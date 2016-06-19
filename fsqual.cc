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
#include <stdint.h>

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

void test_concurrent_append(io_context_t ioctx, int fd, unsigned iodepth, std::string mode) {
    auto nr = 10000;
    auto bufsize = 4096;
    auto ctxsw = 0;
    auto buf = aligned_alloc(4096, 4096);
    auto current_depth = unsigned(0);
    auto initiated = 0;
    auto completed = 0;
    auto iocbs = std::vector<iocb>(iodepth);
    auto iocbps = std::vector<iocb*>(iodepth);
    std::iota(iocbps.begin(), iocbps.end(), iocbs.data());
    auto ioevs = std::vector<io_event>(iodepth);
    while (completed < nr) {
        auto i = unsigned(0);
        while (initiated < nr && current_depth < iodepth) {
            io_prep_pwrite(&iocbs[i++], fd, buf, bufsize, bufsize*initiated++);
            ++current_depth;
        }
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
    std::cout << "context switch per appending io (mode " << mode << ", iodepth " << iodepth << "): " << rate
          << " (" << verdict << ")\n";
    auto ptr = mmap(nullptr, nr * 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    auto incore = std::vector<uint8_t>(nr);
    mincore(ptr, nr * 4096, incore.data());
    if (std::any_of(incore.begin(), incore.end(), [] (uint8_t m) { return m & 1; })) {
        std::cout << "Seen data in page cache (BAD)\n";
    }
}

void run_test(std::function<void (io_context_t ioctx, int fd)> func) {
    io_context_t ioctx = {};
    io_setup(128, &ioctx);
    auto fname = "fsqual.tmp";
    int fd = open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT, 0600);
    unlink(fname);
    func(ioctx, fd);
    close(fd);
    io_destroy(ioctx);
}

int main(int ac, char** av) {
    run_test([] (io_context_t ioctx, int fd) { test_concurrent_append(ioctx, fd, 1, "size-changing"); });
    run_test([] (io_context_t ioctx, int fd) { test_concurrent_append(ioctx, fd, 3, "size-changing"); });
    run_test([] (io_context_t ioctx, int fd) {
        ftruncate(fd, off_t(1) << 30);
        test_concurrent_append(ioctx, fd, 3, "size-unchanging");
    });
    return 0;
}
