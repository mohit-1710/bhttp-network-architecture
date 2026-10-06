# BHTTP/1: HTTP, in binary

Course project for Network Architecture (Scaler School of Technology).
A binary framing of HTTP: a fixed 8-byte frame header, a 10-entry static header table with length-prefixed literals, and a hard rule that **unknown frame types are skipped** so that a version 2 has room to grow.

| Deliverable | Where |
|---|---|
| 1. The spec (two pages) | [`SPEC.md`](SPEC.md) · printable [`docs/SPEC.pdf`](docs/SPEC.pdf) |
| 2. The program: Track 1 server | [`src/bserve.c`](src/bserve.c) |
| 2. The program: Track 2 client | [`src/bcurl.c`](src/bcurl.c) |
| Shared framing / header codec | [`src/bproto.h`](src/bproto.h), [`src/bproto.c`](src/bproto.c) |
| 3. Annotated hexdump of a full request and response | [`HEXDUMP.md`](HEXDUMP.md) |
| Interop tests (an independent Python implementation of the spec) | [`tests/interop.py`](tests/interop.py) |

## Build and run

Needs a C11 compiler and `make`; tested on macOS (clang) and works on Linux (gcc).

```bash
make                                   # builds ./bserve and ./bcurl
./bserve ./www 9000                    # Track 1: serve ./www on port 9000
./bcurl -v localhost:9000/index.html   # Track 2: fetch, hexdump every frame to stderr
```

More client examples:

```bash
./bcurl localhost:9000/index.html /about.html /nope   # 3 requests, ONE connection (exit 4: a 404)
./bcurl -I localhost:9000/img/dot.png                 # HEAD
./bcurl --grease -v localhost:9000/hello.txt          # sends an unknown frame first; server must skip it
./bcurl -H 'x-trace-id: 42' localhost:9000/           # literal (non-table) header
```

`bcurl` exit codes: `0` ok · `1` usage · `2` cannot connect · `3` protocol error · `4` got a 4xx · `5` got a 5xx.

## What each track does

**`bserve <root> <port>`**: accepts TCP connections (one forked process per connection, IPv4 and IPv6). Reads binary request frames, maps `:path` to a file under the root, and replies with a HEADERS frame (`:status`, `server`, `date`, `content-type`, `content-length`) followed by DATA frames of at most 16 KiB each. It returns `404` for a missing file, `400` for a malformed frame or header block, `403` for path traversal and `405` for methods other than GET/HEAD. **It keeps the connection open** for the next request and closes it only after a framing-level error or 30 s idle. `-v` hexdumps frames on the server side too.

**`bcurl [-v] URL [more paths]`**: builds the binary request frame and writes the response body to stdout. `-v` hexdumps every frame sent (`>`) and received (`<`) and decodes header fields. It exits non-zero on 4xx/5xx. **It never opens a second connection**: extra URLs must share the host and port, and they are fetched in order over the same socket.

## Tests

```bash
make test
```

`tests/interop.py` shares no code with `src/`. It re-implements the frame and header codec in Python straight from `SPEC.md` and tests:

* **Python client → `bserve`**: 200/404/400/403/405, HEAD, directory → index.html, unknown frame types and unknown flags skipped, a malformed header block does not kill the connection, a non-increasing stream ID does, request bodies are drained, binary and 300 KB multi-frame files arrive byte-exact, concurrent clients.
* **`bcurl` → Python server**: unknown frames interleaved between DATA frames are skipped, exit codes 0/2/4/5, four requests over exactly one TCP connection (the server counts accepts), and `-v` output.
* **`bcurl` → `bserve`**: end to end, including `--grease`.

Current result: **32 passed, 0 failed**.

> *"A client that only works against your own server is an implementation, not a protocol."*
> That is why the tests run each C program against a peer written separately from the spec, and not only against each other.

## Layout

```
SPEC.md          the protocol (docs/SPEC.pdf = the same, printed: 2 pages)
HEXDUMP.md       annotated bytes of one complete exchange
src/             bproto.[ch] framing + header codec, bserve.c, bcurl.c
www/             sample document root
tests/           interop.py
Makefile
```
