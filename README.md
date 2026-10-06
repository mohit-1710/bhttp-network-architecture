# BHTTP/1: a binary framing for HTTP

Network Architecture course project, Scaler School of Technology.

BHTTP/1 carries HTTP requests and responses as binary frames. Each frame has a fixed 8-byte header (24-bit length, 8-bit type, 8-bit flags, 24-bit stream ID). Header names come from a 10-entry static table or are sent as literals, and receivers skip frame types they do not know (SPEC §3).

| File | What it is |
|---|---|
| [SPEC.md](SPEC.md), [docs/SPEC.pdf](docs/SPEC.pdf) | The protocol specification (the PDF is the same text, two pages) |
| [src/bserve.c](src/bserve.c) | Track 1, the server |
| [src/bcurl.c](src/bcurl.c) | Track 2, the client |
| [src/bproto.c](src/bproto.c), [src/bproto.h](src/bproto.h) | Frame and header-block code used by both |
| [HEXDUMP.md](HEXDUMP.md) | One complete request and response, annotated byte by byte |
| [tests/interop.py](tests/interop.py) | Tests, using a separate Python implementation of the spec |

## Building and running

You need a C11 compiler and make. CI builds and tests on Ubuntu (gcc) and macOS (clang).

```bash
make
./bserve ./www 9000
./bcurl -v localhost:9000/index.html
```

`bserve [-v] [-t seconds] <root> <port>` serves files from `<root>`. `-v` hexdumps frames on the server side. `-t` sets the timeouts (default 30 s): how long an idle connection may wait for the next request, how long a started request has to arrive in full, and how long a write may stall.

`bcurl [-v] [-I] [-X method] [-H 'name: value'] [-t seconds] [--grease] url [more paths]` writes bodies to stdout. `-v` hexdumps every frame to stderr. `-I` sends HEAD. `--grease` sends an unknown frame type before each request to check that the server skips it.

Some other things to try:

```bash
./bcurl localhost:9000/index.html /about.html /nope   # three requests on one connection; exits 4 because of the 404
./bcurl -I localhost:9000/img/dot.png                 # HEAD
./bcurl -v --grease localhost:9000/hello.txt
./bcurl -v -H 'x-trace-id: 7f3a' localhost:9000/hello.txt   # a header outside the static table
```

bcurl exit codes: 0 when every status is below 400, 4 for a 4xx, 5 for a 5xx, 3 for a protocol, timeout or output error, 2 when it cannot connect, 1 for bad arguments.

## How the server behaves

bserve forks a process per connection, up to 64 at a time, and listens on IPv4 and IPv6. It answers each request on the request's stream:

- 200 with the file;
- 404 if there is no file;
- 400 for a malformed header block;
- 403 for `..`, for symlinks that leave the root, and for unreadable files;
- 405 for methods other than GET and HEAD.

After any of these the connection stays open for the next request. When the frame sequence itself is broken, for example a stream ID going backwards or DATA with no request open, it sends a 400 on stream 0 and closes the connection.

## How the client behaves

bcurl opens one connection and sends its requests on it one at a time. Extra arguments must be paths, or URLs for the same host and port. If the server closes the connection, bcurl stops and reports an error instead of reconnecting.

## Tests

```bash
make test
```

[tests/interop.py](tests/interop.py) has its own frame and header-block code written from SPEC.md, so a mistake made the same way in bserve.c and bcurl.c still shows up as a failure. It runs:

- **A Python client against bserve:**
  - every status code, HEAD, pipelining and keep-alive;
  - unknown frame types and flags;
  - malformed and oversized header blocks (including one of exactly 65 536 bytes and one with 300 fields);
  - connection errors on stream 0, truncated frames;
  - path traversal and symlinks out of the root (including a sibling directory whose name starts with the root's name);
  - idle and slow-sender timeouts;
  - a 300 KB file split across DATA frames.
- **bcurl against a Python server:**
  - unknown frames between DATA frames;
  - every exit code;
  - 300 requests counted as one TCP connection by the server;
  - responses on the wrong stream, DATA before HEADERS, a bad `:status`;
  - escaping of control bytes in `-v` output;
  - a server that never answers, and a closed stdout.
- **bcurl against bserve.**

The current result is 62 passed, 0 failed. I also ran the suite with bserve and bcurl built with `-fsanitize=address,undefined`, which reported nothing.

`docs/build.sh` rebuilds the PDF from SPEC.md (needs pandoc and Chrome).
