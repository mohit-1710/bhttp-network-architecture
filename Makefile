CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic

all: bserve bcurl

bserve: src/bserve.c src/bproto.c src/bproto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/bproto.c

# bcurl has its own codec (src/cwire.c) and shares no source with bserve.
bcurl: src/bcurl.c src/cwire.c src/cwire.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/cwire.c

test: all
	python3 tests/interop.py

SANFLAGS = -O1 -g -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra \
           -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

fuzz: tests/fuzz_hb.c src/bproto.c src/bproto.h src/cwire.c src/cwire.h
	$(CC) $(SANFLAGS) -o fuzz_hb tests/fuzz_hb.c src/bproto.c src/cwire.c
	./fuzz_hb 300000

clean:
	rm -rf bserve bcurl fuzz_hb *.dSYM

.PHONY: all test fuzz clean
