#!/usr/bin/env python3
"""
Interop tests for BHTTP/1.

Second implementation of SPEC.md in Python, sharing no code with src/; it
plays client against ./bserve and server against ./bcurl.

    python3 tests/interop.py          (run from the repo root, after `make`)
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
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

def closed_by_peer(sock, wait=5):
    sock.settimeout(wait)
    try:
        return sock.recv(1) == b""
    except ConnectionResetError:
        return True
    except socket.timeout:
        return False

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

SERVER_LOG = os.path.join(tempfile.gettempdir(), f"bserve-test-{os.getpid()}.log")

def start_bserve(*args):
    """Start bserve with its stderr appended to SERVER_LOG (checked at the end)."""
    return subprocess.Popen([BSERVE, *args], stderr=open(SERVER_LOG, "ab"))

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
    srv = start_bserve(WWW, str(port))
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
        check("unknown frames on stream 0 and on the request stream are skipped", st == 200)

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

        # Many fields: only the 64 KiB block limit applies, not a field count.
        many = [(f"x-f{i}", "v") for i in range(300)]
        request(s, 17, "/hi.txt", extra=many)
        st, _, _ = recv_response(s, 17)
        check("300 header fields accepted", st == 200)

        block = enc_headers([(":method", "GET"), (":path", "/hi.txt")])
        while len(block) < 65535:             # pad with literal fields to exactly 65535 bytes
            left = 65535 - len(block)
            n = left - 4 if left - 4 < 128 else min(0x7FFF, left - 5)
            if 0 < 65535 - len(block) - (n + (4 if n < 128 else 5)) < 6:
                n -= 10                        # leave room for one more whole field
            block += enc_headers([("x", "a" * n)])
        s.sendall(frame(HEADERS, END_STREAM, 18, block))
        st, _, _ = recv_response(s, 18)
        check("header block of exactly 65535 bytes accepted", len(block) == 65535 and st == 200)

        s.sendall(frame(HEADERS, END_STREAM, 19, block + b"\x00\x01y\x00"))
        st, _, _ = recv_response(s, 19)
        check("header block over 65535 bytes -> 400", st == 400)

        s.sendall(frame(HEADERS, END_STREAM, 20,
                        enc_headers([("accept", "*/*"), (":method", "GET"), (":path", "/hi.txt")])))
        st, _, _ = recv_response(s, 20)
        check("pseudo-header after a regular field -> 400", st == 400)

        s.sendall(frame(HEADERS, END_STREAM, 21, b"\x01\x80\x03GET\x02\x07/hi.txt"))
        st, _, _ = recv_response(s, 21)
        check("non-minimal two-byte length accepted", st == 200)

        request(s, 22, "/" + "a" * 1100)
        st, _, _ = recv_response(s, 22)
        check(":path over 1024 bytes -> 400", st == 400)

        request(s, 23, "/hi.txt?x=1#frag")
        st, _, body = recv_response(s, 23)
        check("query and fragment ignored", st == 200 and body == b"hi\n")

        request(s, 24, "/bad%zzescape")
        st, _, _ = recv_response(s, 24)
        check("invalid percent escape -> 400", st == 400)

        # Stream id going backwards is a connection error: 400 on stream 0, then close.
        request(s, 3, "/index.html")
        st, _, _ = recv_response(s, 0)
        check("non-increasing stream id -> 400 on stream 0, connection closed",
              st == 400 and closed_by_peer(s))
        s.close()

        s = connect(port)
        s.sendall(frame(DATA, END_STREAM, 7, b"stray"))
        st, _, _ = recv_response(s, 0)
        check("DATA outside a request -> 400 on stream 0, connection closed",
              st == 400 and closed_by_peer(s))
        s.close()

        s = connect(port)
        s.sendall(frame(HEADERS, END_STREAM, 0, enc_headers([(":method", "GET"), (":path", "/")])))
        st, _, _ = recv_response(s, 0)
        check("HEADERS on stream 0 -> 400 on stream 0, connection closed",
              st == 400 and closed_by_peer(s))
        s.close()

        s = connect(port)
        s.sendall(frame(HEADERS, 0, 1, enc_headers([(":method", "GET"), (":path", "/")])))
        s.sendall(frame(HEADERS, END_STREAM, 2, enc_headers([(":method", "GET"), (":path", "/")])))
        st, _, _ = recv_response(s, 0)
        check("new HEADERS while a request body is open -> connection error",
              st == 400 and closed_by_peer(s))
        s.close()

        # A frame cut short by EOF: the server just closes.
        s = connect(port)
        s.sendall(frame(HEADERS, END_STREAM, 1, enc_headers([(":method", "GET"), (":path", "/")]))[:-3])
        s.shutdown(socket.SHUT_WR)
        check("truncated frame then EOF -> server closes", closed_by_peer(s))
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

        # Status precedence: a malformed path is 400 even with an unsupported method.
        s.close()
        s = connect(port)
        request(s, 1, "/" + "a" * 1100, method="POST")
        check("over-long :path with POST -> 400, not 405", recv_response(s, 1)[0] == 400)

        request(s, 2, "/a" * 480)
        check("960-byte path to a missing file -> 404 (no PATH_MAX artefact)",
              recv_response(s, 2)[0] == 404)

        request(s, 3, "/hi.txt", extra=[("host", "a"), ("host", "b")])
        check("repeated host -> 400", recv_response(s, 3)[0] == 400)

        s.sendall(frame(HEADERS, END_STREAM, 4, enc_headers([(":method", "GET"), (":path", "/hi.txt")]) +
                        b"\x00\x03a\"b\x01x"))
        check("literal name with a non-token character -> 400", recv_response(s, 4)[0] == 400)

        request(s, 5, "/nope.html", method="HEAD")
        st, h, body = recv_response(s, 5)
        check("HEAD on a missing file -> 404 with no DATA", st == 404 and body == b"")

        request(s, 7, "/hi.txt", method="G T")
        check(":method that is not a token -> 400", recv_response(s, 7)[0] == 400)

        request(s, 8, "/hi.txt", method="get")
        check("lowercase method is a valid token but not GET -> 405", recv_response(s, 8)[0] == 405)

        s.sendall(frame(HEADERS, END_STREAM, 9, enc_headers([(":method", "GET"), (":path", "/hi.txt"),
                                                             (":status", "200")])))
        check(":status in a request -> 400", recv_response(s, 9)[0] == 400)

        request(s, 12, "/hi.txt", extra=[("content-length", "+0")])
        check("content-length that is not plain digits -> 400", recv_response(s, 12)[0] == 400)

        request(s, 13, "/hi.txt?next=bhttp://x/y")
        check("'://' inside the query is just part of the path", recv_response(s, 13)[0] == 200)
        s.close()

        # Pipelining: two requests before reading either response.
        s = connect(port)
        request(s, 1, "/hi.txt"); request(s, 2, "/about.html")
        a, b = recv_response(s, 1), recv_response(s, 2)
        check("pipelined requests answered in order", a[0] == 200 and b[0] == 200)
        s.close()

        # Two clients at once.
        a, b = connect(port), connect(port)
        request(a, 1, "/hi.txt"); request(b, 1, "/hi.txt")
        check("two concurrent connections", recv_response(a, 1)[0] == 200 and recv_response(b, 1)[0] == 200)
        a.close(); b.close()
    finally:
        srv.terminate()
        srv.wait()

# ------------------------------------------ root containment, timeouts ---

def test_root_and_timeouts():
    print("bserve: root containment and timeouts")
    tmp = tempfile.mkdtemp()
    try:
        root, sib = os.path.join(tmp, "www"), os.path.join(tmp, "www-sib")
        os.mkdir(root); os.mkdir(sib)
        open(os.path.join(root, "ok.txt"), "w").write("ok\n")
        open(os.path.join(sib, "secret.txt"), "w").write("secret\n")
        open(os.path.join(tmp, "outside.txt"), "w").write("outside\n")
        os.symlink(os.path.join(tmp, "outside.txt"), os.path.join(root, "out-link"))
        os.symlink(sib, os.path.join(root, "sib-link"))
        os.symlink("ok.txt", os.path.join(root, "in-link"))
        port = free_port()
        srv = start_bserve("-t", "2", root, str(port))
        try:
            s = connect(port)
            request(s, 1, "/out-link")
            check("symlink to a file outside the root -> 403", recv_response(s, 1)[0] == 403)
            request(s, 2, "/sib-link/secret.txt")
            check("symlink into a sibling dir sharing the root's prefix -> 403",
                  recv_response(s, 2)[0] == 403)
            request(s, 3, "/in-link")
            st, _, body = recv_response(s, 3)
            check("symlink staying inside the root is served", st == 200 and body == b"ok\n")
            s.close()

            open(os.path.join(root, ".env"), "w").write("SECRET=1\n")
            os.symlink(".env", os.path.join(root, "env-link"))
            os.mkfifo(os.path.join(root, "pipe"))
            s = connect(port)
            request(s, 4, "/.env")
            check("dotfile -> 404", recv_response(s, 4)[0] == 404)
            request(s, 5, "/env-link")
            check("symlink to a dotfile -> 404", recv_response(s, 5)[0] == 404)
            os.symlink(tmp, os.path.join(root, "up"))                 # a directory outside
            os.symlink(os.path.join(tmp, "gone.txt"), os.path.join(root, "dangling"))
            request(s, 10, "/up/no-such-file.txt")
            check("missing file behind a symlink that leaves the root -> 403, not 404",
                  recv_response(s, 10)[0] == 403)
            request(s, 11, "/dangling")
            check("dangling symlink pointing outside the root -> 403", recv_response(s, 11)[0] == 403)
            os.symlink("/no-such-top-level-dir-bhttp", os.path.join(root, "dang-top"))
            request(s, 12, "/dang-top")
            check("dangling symlink to a missing top-level path -> 403", recv_response(s, 12)[0] == 403)
            request(s, 120, "/up")
            check("symlink to a directory outside the root -> 403", recv_response(s, 120)[0] == 403)
            request(s, 130, "/pipe")
            s.settimeout(3)
            check("FIFO in the root -> 404 at once, not a hang", recv_response(s, 130)[0] == 404)
            os.makedirs(os.path.join(root, "weird", "index.html"))
            open(os.path.join(root, "weird", "index.html", "index.html"), "w").write("inner\n")
            request(s, 131, "/weird/")
            check("index.html that is a directory -> 404", recv_response(s, 131)[0] == 404)
            os.mkdir(os.path.join(root, "locked"))
            os.chmod(os.path.join(root, "locked"), 0)
            request(s, 132, "/locked/nothere")
            st_locked = recv_response(s, 132)[0]
            os.chmod(os.path.join(root, "locked"), 0o755)
            check("missing file under an unsearchable directory -> 404", os.geteuid() == 0 or st_locked == 404)
            os.symlink("loop-b", os.path.join(root, "loop-a"))
            os.symlink("loop-a", os.path.join(root, "loop-b"))
            request(s, 140, "/loop-a")
            check("symlink loop inside the root -> 404", recv_response(s, 140)[0] == 404)
            request(s, 150, "/x" + "/" * 1000)
            check("missing path made of 1000 slashes -> 404, worker survives",
                  recv_response(s, 150)[0] == 404)
            request(s, 160, "/x/" * 300 + "y")
            check("missing path with 300 segments -> 404", recv_response(s, 160)[0] == 404)
            s.settimeout(None)
            s.close()

            # One client address may hold at most 32 connections.
            held = [connect(port) for _ in range(32)]
            time.sleep(0.3)
            extra = connect(port)
            t0 = time.monotonic()
            check("33rd connection from one address is refused at once",
                  closed_by_peer(extra, wait=2) and time.monotonic() - t0 < 1)
            extra.close()
            for c in held:
                c.close()
            time.sleep(0.5)

            # Closing connections frees their slots: 5 rounds of 16 open/use/close.
            refused = 0
            for _ in range(5):
                conns = [connect(port) for _ in range(16)]
                for c in conns:
                    try:
                        request(c, 1, "/ok.txt")
                        recv_response(c, 1)
                    except (OSError, EOFError):
                        refused += 1
                for c in conns:
                    c.close()
            check("a client that closes its connections is not refused at the cap", refused == 0,
                  f"refused={refused}")

            # A client that reads too slowly is dropped instead of holding a process.
            # (It only sees EOF after draining what the kernel already buffered,
            # so check the server's log rather than the socket.)
            open(os.path.join(root, "big.bin"), "wb").write(b"\0" * 8_000_000)
            s = socket.socket()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            s.connect(("127.0.0.1", port))
            request(s, 1, "/big.bin")
            t0, aborted = time.monotonic(), False
            while time.monotonic() - t0 < 15 and not aborted:
                s.recv(1024)
                time.sleep(0.25)
                aborted = b"client did not take a frame within the timeout" in open(SERVER_LOG, "rb").read()
            check("slow reader is dropped once a frame waits longer than -t", aborted)
            s.close()
            s = connect(port)

            # A bad frame plus one stray byte, then silence: the server must
            # still close soon after reporting the connection error.
            s = connect(port)
            s.sendall(frame(DATA, 0, 1, b"") + b"\x00")
            st, _, _ = recv_response(s, 0)
            t0 = time.monotonic()
            check("after a connection error the server closes even if the client goes quiet",
                  st == 400 and closed_by_peer(s, wait=6) and time.monotonic() - t0 < 4)
            s.close()

            # Idle connection is closed after the timeout (2 s here).
            s = connect(port)
            t0 = time.monotonic()
            closed = closed_by_peer(s, wait=6)
            check("idle connection closed after -t seconds", closed and time.monotonic() - t0 < 5)
            s.close()

            # Trickling one byte at a time does not keep a request alive.
            s = connect(port)
            data = frame(HEADERS, END_STREAM, 1, enc_headers([(":method", "GET"), (":path", "/ok.txt")]))
            t0, dropped = time.monotonic(), False
            for byte in data:
                try:
                    s.sendall(bytes([byte]))
                except OSError:
                    dropped = True
                    break
                time.sleep(0.5)
                if time.monotonic() - t0 > 6:
                    break
            check("slow sender is cut off by the request deadline", dropped or closed_by_peer(s, wait=1))
            s.close()
        finally:
            srv.terminate()
            srv.wait()
    finally:
        shutil.rmtree(tmp)

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

    def drip(self, c, sid):
        """Send a 1-byte unknown frame every 0.4 s and never answer."""
        try:
            for _ in range(40):
                c.sendall(frame(0xF3, 0, 0, b"."))
                time.sleep(0.4)
        except OSError:
            pass

    def empties(self, c, sid):
        """Start a response, then send an empty DATA frame every 0.4 s until bcurl gives up."""
        try:
            c.sendall(frame(HEADERS, 0, sid, enc_headers([(":status", "200")])))
            for _ in range(40):
                c.sendall(frame(DATA, 0, sid, b""))
                time.sleep(0.4)
        except OSError:
            pass

    def handle(self, c):
        try:
            while True:
                t, fl, sid, p = recv_frame(c)
                if t != HEADERS:
                    continue                      # skip unknown
                h = dec_headers(p)
                self.requests.append(h)
                path = h[":path"].split("?")[0]
                if path == "/drip":
                    self.drip(c, sid)
                    continue
                if path == "/empties":
                    self.empties(c, sid)
                    continue
                self.routes.get(path, self.routes["*"])(c, sid)
        except (EOFError, OSError):
            c.close()

    def close(self):
        self.sock.close()

def raw(data_fn):
    def send(c, sid):
        c.sendall(data_fn(sid))
    return send

def resp(status, body, chunks=1, grease=False, extra=()):
    def send(c, sid):
        hb = enc_headers([(":status", str(status)), ("content-length", str(len(body))),
                          ("x-literal-header", "fine")] + list(extra))
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
        "/many": resp(200, b"ok\n", extra=[(f"x-h{i}", "v") for i in range(100)]),
        "/escape": resp(200, b"ok\n", extra=[("x-evil", "\x1b]0;pwned\x07\x1b[2J")]),
        "/plus": raw(lambda sid: frame(HEADERS, END_STREAM, sid, enc_headers([(":status", "+20")]))),
        "/s0": raw(lambda sid: frame(HEADERS, 0, 0, enc_headers([(":status", "400")])) +
                   frame(DATA, END_STREAM, 0, b"400 Bad Request\nyou did something odd\n")),
        "/wrong": raw(lambda sid: frame(HEADERS, END_STREAM, sid + 5, enc_headers([(":status", "200")]))),
        "/data-first": raw(lambda sid: frame(DATA, END_STREAM, sid, b"x")),
        "/silent": raw(lambda sid: b""),
        "/big": resp(200, b"z" * 2_000_000, chunks=200),
        "/short": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "200"),
                                                                      ("content-length", "10")])) +
                      frame(DATA, END_STREAM, sid, b"12345")),
        "/cut": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "200")])) +
                    frame(DATA, 0, sid, b"partial")),
        "/long": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "200"),
                                                                     ("content-length", "3")])) +
                     frame(DATA, END_STREAM, sid, b"x" * 100_000)),
        "/headdata": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "200"),
                                                                         ("content-length", "5")])) +
                         frame(DATA, END_STREAM, sid, b"HELLO")),
        "/midbody": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "200")])) +
                        frame(DATA, 0, sid, b"part") +
                        frame(HEADERS, 0, 0, enc_headers([(":status", "400")])) +
                        frame(DATA, END_STREAM, 0, b"400 Bad Request\nyour second request was odd\n")),
        "/bigreason": raw(lambda sid: frame(HEADERS, 0, 0, enc_headers([(":status", "400")])) +
                          frame(DATA, END_STREAM, 0, b"start of a long reason " + b"x" * 5000)),
        "/head400": raw(lambda sid: frame(HEADERS, 0, sid, enc_headers([(":status", "400"),
                                                                        ("content-length", "4")])) +
                        frame(DATA, END_STREAM, sid, b"bad\n")),
        "/early": raw(lambda sid: frame(HEADERS, END_STREAM, sid, enc_headers([(":status", "103")]))),
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

        r = bcurl(f"{base}/many")
        check("response with 100+ header fields accepted", r.returncode == 0 and r.stdout == b"ok\n")

        r = bcurl("-v", f"{base}/escape")
        check("-v escapes control bytes from the peer", r.returncode == 0 and b"\x1b" not in r.stderr
              and b"\\x1b" in r.stderr)

        r = bcurl(f"{base}/plus")
        check(":status that is not three digits -> exit 3", r.returncode == 3)

        r = bcurl("-v", f"{base}/s0")
        check("400 on stream 0 (connection error) -> exit 3 with the reason",
              r.returncode == 3 and b"you did something odd" in r.stderr)
        check("-v hexdumps the connection-error frames too",
              b"|400 Bad Request.|" in r.stderr and b"[#3 ] :status: 400" in r.stderr)

        r = bcurl(f"{base}/short")
        check("body shorter than content-length -> exit 3", r.returncode == 3)

        r = bcurl("-t", "2", f"{base}/cut")
        check("connection closed mid-body -> exit 3", r.returncode == 3)

        r = bcurl(f"{base}/long")
        check("body longer than content-length -> exit 3, nothing past it written",
              r.returncode == 3 and r.stdout == b"")

        r = bcurl("-I", f"{base}/headdata")
        check("DATA on a HEAD response -> exit 3", r.returncode == 3)

        r = bcurl(f"{base}/midbody")
        check("connection error arriving mid-body is reported with its reason",
              r.returncode == 3 and b"your second request was odd" in r.stderr)

        r = bcurl("-I", f"{base}/empty")
        check("-I prints the response headers", r.returncode == 0 and b":status: 204" in r.stdout)

        r = bcurl("-H", "Host: example.test", f"{base}/ok")
        check("-H host replaces the default instead of repeating it",
              r.returncode == 0 and fs.requests[-1]["host"] == "example.test")

        r = bcurl(f"{base}/" + "a" * 1023)
        check("1024-byte path is accepted", r.returncode == 4 and len(fs.requests[-1][":path"]) == 1024)

        r = bcurl(f"{base}/ok#section")
        check("#fragment is not sent", r.returncode == 0 and fs.requests[-1][":path"] == "/ok")

        t0 = time.monotonic()
        r = bcurl("-t", "2", f"{base}/drip")
        check("unknown frames trickling in do not keep bcurl waiting past -t",
              r.returncode == 3 and time.monotonic() - t0 < 6)

        r = bcurl(f"{base}/bigreason")
        check("a 5 KB connection-error reason is shown (truncated), not dropped",
              r.returncode == 3 and b"start of a long reason" in r.stderr)

        r = bcurl(f"{base}/ok", "/ok#frag")
        check("#fragment is not sent on extra paths either", fs.requests[-1][":path"] == "/ok")

        r = bcurl("-X", "X1", f"{base}/ok")
        check("-X accepts any token", r.returncode == 0 and fs.requests[-1][":method"] == "X1")

        t0 = time.monotonic()
        r = bcurl("-t", "2", f"{base}/empties")
        check("empty DATA frames do not reset bcurl's deadline",
              r.returncode == 3 and time.monotonic() - t0 < 5)

        r = bcurl("127.0.0.1:19101:80/x")
        check("junk in the authority is a usage error", r.returncode == 1)

        r = bcurl("user@127.0.0.1:19101/x")
        check("user@host is a usage error", r.returncode == 1)

        r = bcurl("-I", f"{base}/head400")
        check("a 400 with a body is accepted even for HEAD -> exit 4", r.returncode == 4)

        r = bcurl("-t", "5x", f"{base}/ok")
        check("-t must be a whole number of seconds", r.returncode == 1)

        r = bcurl(f"{base}/early")
        check("1xx status -> exit 3", r.returncode == 3)

        r = bcurl(f"{base}/ok?next=http://example")
        check("'://' in the query is accepted", r.returncode == 0)

        r = bcurl(f"{base}?x=1")
        check("query straight after the authority", r.returncode == 4 and fs.requests[-1][":path"] == "/?x=1")

        r = bcurl("-X", "GE\nT", f"{base}/ok")
        check("-X with a newline -> usage error", r.returncode == 1)

        r = bcurl("127.0.0.1:+9000/x")
        check("port with a sign -> usage error", r.returncode == 1)

        t0 = time.monotonic()
        r = bcurl("-t", "1", "10.255.255.1:9/x")
        check("connect honours -t", r.returncode == 2 and time.monotonic() - t0 < 5)

        r = bcurl(f"{base}/wrong")
        check("response on the wrong stream -> exit 3", r.returncode == 3)

        r = bcurl(f"{base}/data-first")
        check("DATA before HEADERS -> exit 3", r.returncode == 3)

        r = bcurl("-t", "1", f"{base}/silent")
        check("server that never answers -> exit 3 after -t", r.returncode == 3)

        r = subprocess.run(f"{BCURL} {base}/big >&-", shell=True, capture_output=True, timeout=10)
        check("stdout closed -> exit 3", r.returncode == 3)

        before = fs.accepts
        r = bcurl(f"{base}/ok", *(["/ok"] * 299))
        check("300 requests on one connection, none dropped",
              r.returncode == 0 and r.stdout == b"hello world\n" * 300 and fs.accepts - before == 1)

        r = bcurl(f"http://{base}/ok")
        check("http:// scheme -> usage error", r.returncode == 1)

        r = bcurl("-H", "Bad Name: x", f"{base}/ok")
        check("invalid -H name -> usage error", r.returncode == 1)

        r = bcurl("-H", "x-ok: a\nb", f"{base}/ok")
        check("-H value with a newline -> usage error", r.returncode == 1)
    finally:
        fs.close()

# -------------------------------------------------- bcurl -> bserve ------

def test_end_to_end():
    print("bcurl -> bserve")
    port = free_port()
    srv = start_bserve(WWW, str(port))
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
    test_root_and_timeouts()
    test_client()
    test_end_to_end()
    log = open(SERVER_LOG, "rb").read() if os.path.exists(SERVER_LOG) else b""
    check("no sanitizer reports in bserve's stderr", b"Sanitizer" not in log and b"runtime error" not in log)
    if os.path.exists(SERVER_LOG):
        os.remove(SERVER_LOG)
    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)
