CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic

all: bserve bcurl

bserve: src/bserve.c src/bproto.c src/bproto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/bproto.c

bcurl: src/bcurl.c src/bproto.c src/bproto.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/bproto.c

test: all
	python3 tests/interop.py

clean:
	rm -rf bserve bcurl *.dSYM

.PHONY: all test clean
