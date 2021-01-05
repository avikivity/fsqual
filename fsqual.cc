/*
 * Copyright 2016 ScyllaDB
 */


#include <libaio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <type_traits>
#include <functional>
#include <vector>
#include <algorithm>
#include <random>
#include <stdint.h>
#include <sys/vfs.h>
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>

template <typename Counter, typename Func>
typename std::result_of<Func()>::type
with_ctxsw_counting(Counter& counter, Func&& func) {
    struct count_guard {
        Counter& counter;
        count_guard(Counter& counter) : counter(counter) {
            counter -= nvcsw();
        }
        ~count_guard() {
            counter += nvcsw();
        }
        static Counter nvcsw() {
            struct rusage usage;
            getrusage(RUSAGE_THREAD, &usage);
            return usage.ru_nvcsw;
        }
    };
    count_guard g(counter);
    return func();
}

enum class direction {
    read,
    write,
};

void run_test(unsigned iodepth, size_t bufsize, bool pretruncate, bool prezero, bool dsync, direction dir) {
    io_context_t ioctx = {};
    io_setup(128, &ioctx);
    auto fname = "fsqual.tmp";
    auto o_dsync = dsync ? O_DSYNC : 0;
    int fd = open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT|o_dsync, 0600);
    fsxattr attr = {};
    attr.fsx_xflags |= XFS_XFLAG_EXTSIZE;
    attr.fsx_extsize = 32 << 20; // 32MB
    // Ignore error; may be !xfs, and just a hint anyway
    ::ioctl(fd, XFS_IOC_FSSETXATTR, &attr);
    unlink(fname);
    if (pretruncate) {
        ftruncate(fd, off_t(1) << 30);
    }
    auto nr = 10000;
    auto ctxsw = 0;
    auto buf = aligned_alloc(4096, bufsize);
    auto current_depth = unsigned(0);
    auto initiated = 0;
    auto completed = 0;
    auto iocbs = std::vector<iocb>(iodepth);
    auto iocbps = std::vector<iocb*>(iodepth);
    std::iota(iocbps.begin(), iocbps.end(), iocbs.data());
    auto ioevs = std::vector<io_event>(iodepth);
    if (prezero || dir == direction::read) {
        auto buf = reinterpret_cast<char*>(aligned_alloc(4096, nr*bufsize));
        std::fill_n(buf, nr*bufsize, char(0));
        write(fd, buf, nr*bufsize);
        fdatasync(fd);
        free(buf);
    }
    static thread_local std::random_device s_random_device;
    std::default_random_engine random_engine(s_random_device());
    while (completed < nr) {
        auto i = unsigned(0);
        while (initiated < nr && current_depth < iodepth) {
            if (dir == direction::write) {
                io_prep_pwrite(&iocbs[i++], fd, buf, bufsize, bufsize*initiated++);
            } else {
                io_prep_pread(&iocbs[i++], fd, buf, bufsize, bufsize*initiated++);
            }
            ++current_depth;
        }
        std::shuffle(iocbs.begin(), iocbs.begin() + i, random_engine);
        if (i) {
            with_ctxsw_counting(ctxsw, [&] {
                io_submit(ioctx, i, iocbps.data());
            });
        }
        auto n = io_getevents(ioctx, 1, iodepth, ioevs.data(), nullptr);
        current_depth -= n;
        completed += n;
    }
    auto rate = float(ctxsw) / nr;
    auto verdict = rate < 0.1 ? "GOOD" : "BAD";
    auto mode = std::string(pretruncate ? "size-unchanging" : "size-changing");
    if (prezero) {
        mode += ", overwrite";
    } else {
        mode += ", append";
    }
    mode += ", blocksize " + std::to_string(bufsize);
    if (dsync) {
        mode += ", O_DSYNC";
    }
    auto iotype = dir == direction::read ? "read" : "write";
    std::cout << "context switch per " << iotype << " io (" << mode << ", iodepth " << iodepth << "): " << rate
          << " (" << verdict << ")\n";
    auto ptr = mmap(nullptr, nr * 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    auto incore = std::vector<uint8_t>(nr);
    mincore(ptr, nr * 4096, incore.data());
    if (std::any_of(incore.begin(), incore.end(), [] (uint8_t m) { return m & 1; })) {
        std::cout << "Seen data in page cache (BAD)\n";
    }
    close(fd);
    io_destroy(ioctx);
}

struct dio_info {
    size_t memory_alignment;
    size_t disk_alignment;
};

dio_info get_dio_info() {
    auto fname = "fsqual.tmp";
    int fd = open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT, 0600);
    if (fd == -1) {
        std::cout << "failed to create file\n";
        return {512, 512};
    }
    unlink(fname);
    struct dioattr da;
    auto r = ioctl(fd, XFS_IOC_DIOINFO, &da);
    if (r == -1) {
        std::cout << "XFS_IOC_DIOINFO failed, guessing alignment\n";
        return dio_info{512, 512};
    }
    return dio_info{da.d_mem, da.d_miniosz};
}

unsigned get_blocksize() {
    struct statfs s;
    statfs(".", &s);
    return s.f_bsize;
}

int main(int ac, char** av) {
    auto info = get_dio_info();
    auto bsize = get_blocksize();
    std::cout << "memory DMA alignment:    " << info.memory_alignment << "\n";
    std::cout << "disk DMA alignment:      " << info.disk_alignment << "\n";
    std::cout << "filesystem block size:   " << bsize << "\n";

    run_test(1, bsize, false, false, false, direction::write);
    run_test(3, bsize, false, false, false, direction::write);
    run_test(3, bsize, true, false, false, direction::write);
    run_test(7, bsize, true, false, false, direction::write);
    run_test(1, info.disk_alignment, true, false, false, direction::write);
    run_test(1, info.disk_alignment, true, true, false, direction::write);
    run_test(1, info.disk_alignment, true, true, true, direction::write);
    run_test(3, info.disk_alignment, true, true, true, direction::write);
    run_test(3, info.disk_alignment, true, true, true, direction::write);
    run_test(1, bsize, false, false, false, direction::write);
    run_test(3, bsize, false, false, false, direction::write);
    run_test(3, bsize, true, false, false, direction::write);
    run_test(7, bsize, true, false, false, direction::write);
    run_test(1, info.disk_alignment, true, false, false, direction::write);
    run_test(1, info.disk_alignment, true, true, false, direction::write);
    run_test(1, info.disk_alignment, true, true, true, direction::write);
    run_test(3, info.disk_alignment, true, true, true, direction::write);
    run_test(30, info.disk_alignment, false, false, false, direction::read);
    return 0;
}
