#!/usr/bin/env python3
"""
Interop tests for BHTTP/1.

This file is a second, independent implementation of the spec written in
Python (it shares no code with src/). It plays client against ./bserve and
server against ./bcurl, so the C programs are tested against the *protocol*,
not just against each other.

    python3 tests/interop.py          (run from the repo root, after `make`)
"""
import os
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WWW = os.path.join(ROOT, "www")
BSERVE = os.path.join(ROOT, "bserve")
BCURL = os.path.join(ROOT, "bcurl")

DATA, HEADERS = 0x00, 0x01
END_STREAM = 0x01
STATIC = [None, ":method", ":path", ":status", "host", "user-agent", "accept",
          "server", "date", "content-type", "content-length"]

# ---------------------------------------------------------------- codec ---

def frame(ftype, flags, stream, payload=b""):
    n = len(payload)
    return struct.pack(">BHBBBH", n >> 16, n & 0xFFFF, ftype, flags,
                       stream >> 16, stream & 0xFFFF) + payload

def enc_len(n):
    assert n <= 0x7FFF
    return bytes([n]) if n < 0x80 else bytes([0x80 | (n >> 8), n & 0xFF])

def enc_headers(fields):
    out = b""
    for name, value in fields:
        v = value.encode()
        if name in STATIC:
            out += bytes([STATIC.index(name)])
        else:
            out += b"\x00" + enc_len(len(name)) + name.encode()
        out += enc_len(len(v)) + v
    return out

def dec_headers(b):
    fields, i = [], 0
    def take_len():
        nonlocal i
        x = b[i]; i += 1
        if x & 0x80:
            x = (x & 0x7F) << 8 | b[i]; i += 1
        return x
    while i < len(b):
        tag = b[i]; i += 1
        if tag == 0:
            n = take_len(); name = b[i:i + n].decode(); i += n
        else:
            name = STATIC[tag]
        n = take_len(); value = b[i:i + n].decode(); i += n
        fields.append((name, value))
    return dict(fields)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError(f"EOF after {len(buf)}/{n} bytes")
        buf += chunk
    return buf

def recv_frame(sock):
    h = recv_exact(sock, 8)
    length = h[0] << 16 | h[1] << 8 | h[2]
    stream = h[5] << 16 | h[6] << 8 | h[7]
    return h[3], h[4], stream, recv_exact(sock, length)

def recv_response(sock, sid):
    """Read one response, skipping unknown frame types as the spec requires."""
    while True:
        t, fl, s, p = recv_frame(sock)
        if t in (DATA, HEADERS):
            break
    assert t == HEADERS and s == sid, (t, s)
    hdrs, body = dec_headers(p), b""
    while not fl & END_STREAM:
        t, fl, s, p = recv_frame(sock)
        if t not in (DATA, HEADERS):
            continue
        assert t == DATA and s == sid
        body += p
    return int(hdrs[":status"]), hdrs, body

def request(sock, sid, path, method="GET", extra=()):
    fields = [(":method", method), (":path", path), ("host", "localhost"),
              ("user-agent", "interop.py")] + list(extra)
    sock.sendall(frame(HEADERS, END_STREAM, sid, enc_headers(fields)))

# ---------------------------------------------------------- harness ------

PASS = FAIL = 0

def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"  ok    {name}")
    else:
        FAIL += 1
        print(f"  FAIL  {name}  {detail}")

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

def connect(port):
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            return s
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("bserve did not start")

# ------------------------------------------- python client -> bserve -----

def test_server():
    print("python client -> bserve")
    port = free_port()
    srv = subprocess.Popen([BSERVE, WWW, str(port)], stderr=subprocess.DEVNULL)
    try:
        s = connect(port)
        index = open(os.path.join(WWW, "index.html"), "rb").read()

        request(s, 1, "/index.html")
        st, h, body = recv_response(s, 1)
        check("GET /index.html -> 200 with exact bytes", st == 200 and body == index)
        check("content-length and content-type present",
              h.get("content-length") == str(len(index)) and h.get("content-type", "").startswith("text/html"))

        request(s, 2, "/does-not-exist.html")
        st, _, _ = recv_response(s, 2)
        check("missing file -> 404", st == 404)

        request(s, 3, "/")
        st, _, body = recv_response(s, 3)
        check("directory maps to index.html, same connection", st == 200 and body == index)

        # Unknown frame types, on stream 0 and on a request stream, must be skipped.
        s.sendall(frame(0x7F, 0xFF, 0, b"x" * 300))
        s.sendall(frame(0xFA, 0, 4, b"from the future"))
        request(s, 4, "/about.html")
        st, _, _ = recv_response(s, 4)
        check("unknown frame types are skipped cleanly", st == 200)

        # Unknown flag bits are ignored.
        s.sendall(frame(HEADERS, END_STREAM | 0xF0, 5,
                        enc_headers([(":method", "GET"), (":path", "/hi.txt")])))
        st, _, body = recv_response(s, 5)
        check("unknown flag bits are ignored", st == 200 and body == b"hi\n")

        # Malformed header block (reserved table index 0x0B) -> 400, connection stays open.
        s.sendall(frame(HEADERS, END_STREAM, 6, b"\x0b\x03GET"))
        st, _, _ = recv_response(s, 6)
        check("reserved static index -> 400", st == 400)

        s.sendall(frame(HEADERS, END_STREAM, 7, b"\x01\x05GE"))   # length runs past the end
        st, _, _ = recv_response(s, 7)
        check("truncated string in header block -> 400", st == 400)

        s.sendall(frame(HEADERS, END_STREAM, 8, enc_headers([(":path", "/index.html")])))
        st, _, _ = recv_response(s, 8)
        check("missing :method -> 400", st == 400)

        request(s, 9, "/index.html")
        st, _, _ = recv_response(s, 9)
        check("connection still usable after 400s", st == 200)

        request(s, 10, "/index.html", method="POST")
        st, _, _ = recv_response(s, 10)
        check("POST -> 405", st == 405)

        request(s, 11, "/../../etc/passwd")
        st, _, _ = recv_response(s, 11)
        check("path traversal -> 403", st == 403)

        request(s, 12, "/%2e%2e/%2e%2e/etc/passwd")
        st, _, _ = recv_response(s, 12)
        check("percent-encoded traversal -> 403", st == 403)

        request(s, 13, "/index.html", method="HEAD")
        st, h, body = recv_response(s, 13)
        check("HEAD -> 200, length header, no body",
              st == 200 and body == b"" and h["content-length"] == str(len(index)))

        # A request with a body: server must drain it, then answer.
        s.sendall(frame(HEADERS, 0, 14, enc_headers([(":method", "GET"), (":path", "/hi.txt")])))
        s.sendall(frame(DATA, 0, 14, b"a" * 100) + frame(0x42, 0, 0, b"skip") +
                  frame(DATA, END_STREAM, 14, b"b"))
        st, _, body = recv_response(s, 14)
        check("request body drained (with an unknown frame inside it)", st == 200 and body == b"hi\n")

        request(s, 15, "/img/dot.png")
        st, h, body = recv_response(s, 15)
        check("binary file byte-exact", st == 200 and body == open(os.path.join(WWW, "img/dot.png"), "rb").read()
              and h["content-type"] == "image/png")

        # Literal (non-table) header names are accepted.
        request(s, 16, "/hi.txt", extra=[("x-trace-id", "abc123")])
        st, _, _ = recv_response(s, 16)
        check("literal header name accepted", st == 200)

        # Stream id going backwards is a connection error: 400, then close.
        request(s, 3, "/index.html")
        st, _, _ = recv_response(s, 3)
        s.settimeout(3)
        closed = s.recv(1) == b""
        check("non-increasing stream id -> 400 and connection closed", st == 400 and closed)
        s.close()

        # Large file spans many DATA frames.
        big = os.path.join(WWW, "big.bin")
        data = bytes((i * 7) & 0xFF for i in range(300_000))
        open(big, "wb").write(data)
        try:
            s = connect(port)
            request(s, 1, "/big.bin")
            frames, body = 0, b""
            t, fl, sid, p = recv_frame(s)
            while not fl & END_STREAM:
                t, fl, sid, p = recv_frame(s)
                frames += 1
                body += p
            check(f"300 KB file arrives intact in {frames} DATA frames", body == data and frames > 1)
            s.close()
        finally:
            os.remove(big)

        # Two clients at once.
        a, b = connect(port), connect(port)
        request(a, 1, "/hi.txt"); request(b, 1, "/hi.txt")
        check("two concurrent connections", recv_response(a, 1)[0] == 200 and recv_response(b, 1)[0] == 200)
        a.close(); b.close()
    finally:
        srv.terminate()
        srv.wait()

# -------------------------------------------- bcurl -> python server -----

class FakeServer:
    """Serves canned responses and counts connections."""

    def __init__(self, routes):
        self.routes, self.accepts, self.requests = routes, 0, []
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self.accepts += 1
            threading.Thread(target=self.handle, args=(c,), daemon=True).start()

    def handle(self, c):
        try:
            while True:
                t, fl, sid, p = recv_frame(c)
                if t != HEADERS:
                    continue                      # skip unknown
                h = dec_headers(p)
                self.requests.append(h)
                self.routes.get(h[":path"], self.routes["*"])(c, sid)
        except (EOFError, OSError):
            c.close()

    def close(self):
        self.sock.close()

def resp(status, body, chunks=1, grease=False):
    def send(c, sid):
        hb = enc_headers([(":status", str(status)), ("content-length", str(len(body))),
                          ("x-literal-header", "fine")])
        out = frame(0xEE, 0, 0, b"v2 says hello") if grease else b""
        out += frame(HEADERS, 0 if body else END_STREAM, sid, hb)
        if body:
            step = max(1, len(body) // chunks)
            parts = [body[i:i + step] for i in range(0, len(body), step)]
            for i, part in enumerate(parts):
                if grease:
                    out += frame(0xEF, 0x80, sid, b"\x00" * 17)
                out += frame(DATA, END_STREAM if i == len(parts) - 1 else 0, sid, part)
        c.sendall(out)
    return send

def bcurl(*args):
    return subprocess.run([BCURL, *args], capture_output=True, timeout=10)

def test_client():
    print("bcurl -> python server")
    fs = FakeServer({
        "/ok": resp(200, b"hello world\n", chunks=3, grease=True),
        "/empty": resp(204, b""),
        "/teapot": resp(418, b"short and stout\n"),
        "/boom": resp(503, b"down\n"),
        "*": resp(404, b"nope\n"),
    })
    try:
        base = f"127.0.0.1:{fs.port}"
        r = bcurl(f"{base}/ok")
        check("body to stdout, unknown frames between DATA skipped",
              r.returncode == 0 and r.stdout == b"hello world\n", r)
        check("request carries :method/:path/host",
              fs.requests[-1][":method"] == "GET" and fs.requests[-1]["host"] == base)

        r = bcurl(f"{base}/empty")
        check("HEADERS with END_STREAM and no DATA -> exit 0, empty body", r.returncode == 0 and r.stdout == b"")

        r = bcurl(f"bhttp://{base}/teapot")
        check("4xx -> exit 4 (body still printed)", r.returncode == 4 and r.stdout == b"short and stout\n")

        r = bcurl(f"{base}/boom")
        check("5xx -> exit 5", r.returncode == 5)

        before = fs.accepts
        r = bcurl(f"{base}/ok", "/ok", "/teapot", f"{base}/ok")
        check("4 requests over exactly one connection",
              fs.accepts - before == 1 and r.stdout == b"hello world\n" * 2 + b"short and stout\n" + b"hello world\n",
              f"accepts={fs.accepts - before}")
        check("…and exit reflects the 4xx", r.returncode == 4)

        r = bcurl("-v", f"{base}/ok")
        err = r.stderr.decode()
        check("-v hexdumps sent and received frames",
              "> HEADERS" in err and "< DATA" in err and "< UNKNOWN" in err and "skipped" in err)

        r = bcurl(f"{base}/ok", "127.0.0.2:1/x")
        check("refuses a second host (no second connection)", r.returncode == 1)

        r = bcurl("127.0.0.1:1/x")
        check("connection refused -> exit 2", r.returncode == 2)
    finally:
        fs.close()

# -------------------------------------------------- bcurl -> bserve ------

def test_end_to_end():
    print("bcurl -> bserve")
    port = free_port()
    srv = subprocess.Popen([BSERVE, WWW, str(port)], stderr=subprocess.DEVNULL)
    try:
        connect(port).close()
        r = bcurl("--grease", f"localhost:{port}/index.html")
        check("--grease request (unknown frame first) served",
              r.returncode == 0 and r.stdout == open(os.path.join(WWW, "index.html"), "rb").read())
        r = bcurl(f"localhost:{port}/missing")
        check("404 -> exit 4", r.returncode == 4)
    finally:
        srv.terminate()
        srv.wait()

if __name__ == "__main__":
    for exe in (BSERVE, BCURL):
        if not os.access(exe, os.X_OK):
            sys.exit(f"{exe} not built; run `make` first")
    test_server()
    test_client()
    test_end_to_end()
    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)
