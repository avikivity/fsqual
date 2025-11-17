CXXFLAGS = -O2 -g -std=gnu++11 -isystem external/tabulate/include -Wall -Wextra
LDLIBS = -laio

fsqual: fsqual.cc
