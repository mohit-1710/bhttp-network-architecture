# Annotated hexdump: one complete request and response

Captured with `./bcurl -v localhost:9000/hello.txt` against `./bserve ./www 9000`.
`www/hello.txt` holds the 20 bytes `Hello, binary HTTP!\n`.
The whole exchange is 3 frames and 172 bytes: a 57-byte request and a 115-byte response (87 + 28).

## Request: client → server (57 bytes, 1 frame)

```
00 00 31 01 01 00 00 01  01 03 47 45 54 02 0a 2f
68 65 6c 6c 6f 2e 74 78  74 04 0e 6c 6f 63 61 6c
68 6f 73 74 3a 39 30 30  30 05 09 62 63 75 72 6c
2f 31 2e 30 06 03 2a 2f  2a
```

| offset | bytes | meaning |
|---|---|---|
| | **frame header** | |
| 0x00 | `00 00 31` | Length = 0x31 = **49** payload bytes follow |
| 0x03 | `01` | Type = **HEADERS** |
| 0x04 | `01` | Flags = **END_STREAM**: no body follows (a GET) |
| 0x05 | `00 00 01` | Stream ID = **1**, the first request on this connection |
| | **header block** (49 bytes) | |
| 0x08 | `01` | tag 1 → static name `:method` |
| 0x09 | `03` | value length 3 (one-byte form, top bit 0) |
| 0x0a | `47 45 54` | `GET` |
| 0x0d | `02` | tag 2 → `:path` |
| 0x0e | `0a` | value length 10 |
| 0x0f | `2f 68 65 6c 6c 6f 2e 74 78 74` | `/hello.txt` |
| 0x19 | `04` | tag 4 → `host` |
| 0x1a | `0e` | value length 14 |
| 0x1b | `6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30` | `localhost:9000` |
| 0x29 | `05` | tag 5 → `user-agent` |
| 0x2a | `09` | value length 9 |
| 0x2b | `62 63 75 72 6c 2f 31 2e 30` | `bcurl/1.0` |
| 0x34 | `06` | tag 6 → `accept` |
| 0x35 | `03` | value length 3 |
| 0x36 | `2a 2f 2a` | `*/*`, the last byte of the request (offset 0x38 = 56) |

Check: 5 fields cost (1+1+3) + (1+1+10) + (1+1+14) + (1+1+9) + (1+1+3) = 49 = 0x31. ✓
Every name came from the static table, so no name strings are on the wire.

## Response: server → client (115 bytes, 2 frames)

### Frame 1: HEADERS (8 + 79 = 87 bytes)

```
00 00 4f 01 00 00 00 01  03 03 32 30 30 07 0a 62
73 65 72 76 65 2f 31 2e  30 08 1d 54 75 65 2c 20
30 36 20 4f 63 74 20 32  30 32 36 20 32 30 3a 31
39 3a 32 30 20 47 4d 54  09 19 74 65 78 74 2f 70
6c 61 69 6e 3b 20 63 68  61 72 73 65 74 3d 75 74
66 2d 38 0a 02 32 30
```

| offset | bytes | meaning |
|---|---|---|
| | **frame header** | |
| 0x00 | `00 00 4f` | Length = 0x4f = **79** |
| 0x03 | `01` | Type = **HEADERS** |
| 0x04 | `00` | Flags = none, so **DATA follows** |
| 0x05 | `00 00 01` | Stream ID = **1**: this answers request 1 |
| | **header block** (79 bytes) | |
| 0x08 | `03` `03` `32 30 30` | tag 3 `:status`, length 3, `200` |
| 0x0d | `07` `0a` `62 73 65 72 76 65 2f 31 2e 30` | tag 7 `server`, length 10, `bserve/1.0` |
| 0x19 | `08` `1d` `54 75 65 … 47 4d 54` | tag 8 `date`, length 0x1d = 29, `Tue, 06 Oct 2026 20:19:20 GMT` |
| 0x38 | `09` `19` `74 65 78 74 … 75 74 66 2d 38` | tag 9 `content-type`, length 0x19 = 25, `text/plain; charset=utf-8` |
| 0x53 | `0a` `02` `32 30` | tag 10 `content-length`, length 2, `20` |

Check: (2+3) + (2+10) + (2+29) + (2+25) + (2+2) = 79 = 0x4f. ✓

### Frame 2: DATA (8 + 20 = 28 bytes)

```
00 00 14 00 01 00 00 01  48 65 6c 6c 6f 2c 20 62
69 6e 61 72 79 20 48 54  54 50 21 0a
```

| offset | bytes | meaning |
|---|---|---|
| 0x00 | `00 00 14` | Length = 0x14 = **20** |
| 0x03 | `00` | Type = **DATA** |
| 0x04 | `01` | Flags = **END_STREAM**: the response is complete |
| 0x05 | `00 00 01` | Stream ID = **1** |
| 0x08 | `48 65 6c 6c 6f 2c 20 62 69 6e 61 72 79 20 48 54 54 50 21 0a` | `Hello, binary HTTP!\n`, copied to stdout as-is |

After END_STREAM the client could send stream 2 on the same socket. Here it had nothing more to fetch, so it closed the connection and exited 0, since 200 is not 4xx/5xx.

## Bonus: an unknown frame being skipped

`./bcurl -v --grease localhost:9000/hello.txt` sends this frame before the request:

```
00 00 1d fa 00 00 00 00  75 6e 6b 6e 6f 77 6e 20 ...   "unknown frame, please skip me"
└Length=29 └Type=0xFA (unassigned) └Flags=0 └Stream=0
```

The server does not know type 0xFA. It reads the 29 payload bytes it was told about, discards them, and then reads the HEADERS frame that follows as normal. The response is identical to the one above. This is the §3 rule in action, and `tests/interop.py` checks it in both directions.

## Reproduce

```bash
make
./bserve ./www 9000 &
./bcurl -v localhost:9000/hello.txt
```
