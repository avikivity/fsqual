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

int main(int ac, char** av) {
    io_context_t ioctx = {};
    io_setup(1, &ioctx);
    auto fname = "fsqual.tmp";
    int fd = open(fname, O_CREAT|O_EXCL|O_RDWR|O_DIRECT, 0600);
    unlink(fname);
    auto nr = 1000;
    auto bufsize = 4096;
    auto ctxsw = 0;
    auto buf = aligned_alloc(4096, 4096);
    for (int i = 0; i < nr; ++i) {
        struct iocb cmd;
        io_prep_pwrite(&cmd, fd, buf, bufsize, bufsize*i);
        struct iocb* cmds[1] = { &cmd };
        with_ctxsw_counting(ctxsw, [&] {
            io_submit(ioctx, 1, cmds);
        });
        struct io_event ioev;
        io_getevents(ioctx, 1, 1, &ioev, nullptr);
    }
    auto rate = float(ctxsw) / nr;
    auto verdict = rate < 0.1 ? "GOOD" : "BAD";
    std::cout << "context switch per appending io: " << rate
	      << " (" << verdict << ")\n";
    return 0;
}
