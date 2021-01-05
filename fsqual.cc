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

void test_concurrent_append(io_context_t ioctx, int fd, unsigned iodepth, size_t bufsize, bool pretruncate, bool prezero) {
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
    if (prezero) {
        auto buf = reinterpret_cast<char*>(aligned_alloc(4096, nr*bufsize));
        std::fill_n(buf, nr*bufsize, char(0));
        write(fd, buf, nr*bufsize);
        fdatasync(fd);
        free(buf);
    }
    std::random_device random_device;
    while (completed < nr) {
        auto i = unsigned(0);
        while (initiated < nr && current_depth < iodepth) {
            io_prep_pwrite(&iocbs[i++], fd, buf, bufsize, bufsize*initiated++);
            ++current_depth;
        }
        std::shuffle(iocbs.begin(), iocbs.begin() + i, random_device);
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
        mode += ", prezero";
    }
    mode += ", blocksize " + std::to_string(bufsize);
    std::cout << "context switch per appending io (mode " << mode << ", iodepth " << iodepth << "): " << rate
          << " (" << verdict << ")\n";
    auto ptr = mmap(nullptr, nr * 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    auto incore = std::vector<uint8_t>(nr);
    mincore(ptr, nr * 4096, incore.data());
    if (std::any_of(incore.begin(), incore.end(), [] (uint8_t m) { return m & 1; })) {
        std::cout << "Seen data in page cache (BAD)\n";
    }
}

void run_test(unsigned iodepth, size_t bufsize, bool pretruncate, bool prezero) {
    io_context_t ioctx = {};
    io_setup(128, &ioctx);
    auto fname = "fsqual.tmp";
    int fd = open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT, 0600);
    fsxattr attr = {};
    attr.fsx_xflags |= XFS_XFLAG_EXTSIZE;
    attr.fsx_extsize = 32 << 20; // 32MB
    // Ignore error; may be !xfs, and just a hint anyway
    ::ioctl(fd, XFS_IOC_FSSETXATTR, &attr);
    unlink(fname);
    test_concurrent_append(ioctx, fd, iodepth, bufsize, pretruncate, prezero);
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

int main(int ac, char** av) {
    auto info = get_dio_info();
    std::cout << "memory DMA alignment:    " << info.memory_alignment << "\n";
    std::cout << "disk DMA alignment:      " << info.disk_alignment << "\n";

    run_test(1, 4096, false, false);
    run_test(3, 4096, false, false);
    run_test(3, 4096, true, false);
    run_test(7, 4096, true, false);
    run_test(1, info.disk_alignment, true, false);
    run_test(1, info.disk_alignment, true, true);
    return 0;
}
