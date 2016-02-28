/*
 * Copyright 2016 ScyllaDB
 */


#include <libaio.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <type_traits>
#include <functional>
#include <vector>
#include <algorithm>

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

void test_concurrent_append(io_context_t ioctx, int fd, unsigned iodepth) {
    auto nr = 1000;
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
    std::cout << "context switch per appending io (iodepth " << iodepth << "): " << rate
          << " (" << verdict << ")\n";
}

void test_append(io_context_t ioctx, int fd) {
    return test_concurrent_append(ioctx, fd, 1);
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
    run_test(test_append);
    run_test([] (io_context_t ioctx, int fd) { test_concurrent_append(ioctx, fd, 3); });
    run_test([] (io_context_t ioctx, int fd) {
        ftruncate(fd, off_t(1) << 30);
        test_concurrent_append(ioctx, fd, 3);
    });
    return 0;
}
