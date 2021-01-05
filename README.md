# fsqual - file system qualification tool for asynchonus I/O

## About

Linux AIO is notoriously bad; but it is hard to understand what the weaknesses of a filesystem implementation in a particular kernel version are. The `fsqual` tool provides a way to qualify a filesystem's aio implementation.

See this [blog post](http://www.scylladb.com/2016/02/09/qualifying-filesystems/) for more information.

The test matrix checks the following variations:
 * Whether the write changes the size of the file or not
 * Whether the write first touches its offset range, or alternatively overwrites already-written data
 * Whether the write uses sector granularity (typically 512 bytes) or block granularity (typically 4096 bytes)
 * Whether the writes happen concurrently, or only one at a time
 * Whether O_DSYNC is in use (as is typical for commitlogs) or not

## Building

Install the following packages:
 * `libaio-devel` (`libaio-dev` on Debian and derivatives)
 * `xfsprogs-devel.x86_64` (`xfslibs-dev` on Debian and derivatives)
 * `gcc-c++` (`g++` on Debian and derivatives)
 * `make`

To build, simply run

    make

## Running

Change to a directory under the mountpoint to be tested, and run `fsqual`.

`fsqual` will report, for each test scenario, whether `io_submit` was truly asynchronous. This is done by measuring context switches during the `io_submit` call.
