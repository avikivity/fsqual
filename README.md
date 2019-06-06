# fsqual - file system qualification tool for asynchonus I/O

## About

Linux AIO is notoriously bad; but it is hard to understand what the weaknesses of a filesystem implementation in a particular kernel version are. The `fsqual` tool provides a way to qualify a filesystem's aio implementation.

See this [blog post](http://www.scylladb.com/2016/02/09/qualifying-filesystems/) for more information.

Currently tested attributes are:
 * Asycnhronous appending write
 * Asycnhronous allocating, but non-appending write

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

