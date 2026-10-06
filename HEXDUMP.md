# Annotated hexdump of one request and response

This is one exchange between `./bcurl` and `./bserve ./www 9000`. The request asks for `/hello.txt`, a 20-byte file containing `Hello, binary HTTP!\n`. I added one header that is not in the static table (`-H 'x-trace-id: 7f3a'`) so the dump also shows a literal name:

```
./bcurl -v -H 'x-trace-id: 7f3a' localhost:9000/hello.txt
```

On the wire there are three frames and 189 bytes: a 74-byte request and a 115-byte response.

Offsets in the tables count from the first payload byte, as in the `-v` output. Payload offset 0x00 is byte 8 of the frame. The unedited `-v` output is at the end of this file.

## Request, client to server (one frame, 8 + 66 bytes)

```
header   00 00 42 01 01 00 00 01
payload  01 03 47 45 54 02 0a 2f  68 65 6c 6c 6f 2e 74 78
         74 04 0e 6c 6f 63 61 6c  68 6f 73 74 3a 39 30 30
         30 05 09 62 63 75 72 6c  2f 31 2e 30 06 03 2a 2f
         2a 00 0a 78 2d 74 72 61  63 65 2d 69 64 04 37 66
         33 61
```

Frame header:

| bytes | field | meaning |
|---|---|---|
| `00 00 42` | Length | 0x42 = 66 payload bytes follow |
| `01` | Type | HEADERS |
| `01` | Flags | END_STREAM, so no DATA follows (a GET has no body) |
| `00 00 01` | Stream ID | 1, the first request on this connection |

Header block. Each field is a tag byte, then a literal name only when the tag is 0, then a length-prefixed value:

| offset | bytes | decoded |
|---|---|---|
| 0x00 | `01` `03` `47 45 54` | tag 1 `:method`, length 3, `GET` |
| 0x05 | `02` `0a` `2f 68 65 6c 6c 6f 2e 74 78 74` | tag 2 `:path`, length 10, `/hello.txt` |
| 0x11 | `04` `0e` `6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30` | tag 4 `host`, length 14, `localhost:9000` |
| 0x21 | `05` `09` `62 63 75 72 6c 2f 31 2e 30` | tag 5 `user-agent`, length 9, `bcurl/1.0` |
| 0x2c | `06` `03` `2a 2f 2a` | tag 6 `accept`, length 3, `*/*` |
| 0x31 | `00` | tag 0: the name is a literal |
| 0x32 | `0a` `78 2d 74 72 61 63 65 2d 69 64` | name length 10, `x-trace-id` |
| 0x3d | `04` `37 66 33 61` | value length 4, `7f3a`, which ends at 0x41 (the 66th byte) |

The pseudo-headers come first, as §5 requires. The five table fields cost 5 + 12 + 16 + 11 + 5 = 49 bytes. The literal costs 1 + 1 + 10 + 1 + 4 = 17, because it carries its name. 49 + 17 = 66 = 0x42, the Length in the header. Without `-H` the same request is 57 bytes; the spec's §8 compares that with the 85-byte HTTP/1.1 text.

## Response, server to client (two frames, 87 + 28 bytes)

### HEADERS frame

```
header   00 00 4f 01 00 00 00 01
payload  03 03 32 30 30 07 0a 62  73 65 72 76 65 2f 31 2e
         30 08 1d 54 75 65 2c 20  30 36 20 4f 63 74 20 32
         30 32 36 20 32 30 3a 34  32 3a 35 38 20 47 4d 54
         09 19 74 65 78 74 2f 70  6c 61 69 6e 3b 20 63 68
         61 72 73 65 74 3d 75 74  66 2d 38 0a 02 32 30
```

| bytes | field | meaning |
|---|---|---|
| `00 00 4f` | Length | 0x4f = 79 |
| `01` | Type | HEADERS |
| `00` | Flags | END_STREAM not set, so DATA frames follow |
| `00 00 01` | Stream ID | 1: this answers request 1 |

| offset | bytes | decoded |
|---|---|---|
| 0x00 | `03` `03` `32 30 30` | tag 3 `:status`, length 3, `200` |
| 0x05 | `07` `0a` `62 73 65 72 76 65 2f 31 2e 30` | tag 7 `server`, length 10, `bserve/1.0` |
| 0x11 | `08` `1d` `54 75 65 2c 20 30 36 20 4f 63 74 20 32 30 32 36 20 32 30 3a 34 32 3a 35 38 20 47 4d 54` | tag 8 `date`, length 0x1d = 29, `Tue, 06 Oct 2026 20:42:58 GMT` |
| 0x30 | `09` `19` `74 65 78 74 2f 70 6c 61 69 6e 3b 20 63 68 61 72 73 65 74 3d 75 74 66 2d 38` | tag 9 `content-type`, length 0x19 = 25, `text/plain; charset=utf-8` |
| 0x4b | `0a` `02` `32 30` | tag 10 `content-length`, length 2, `20` |

The sizes are 5 + 12 + 31 + 27 + 4 = 79 = 0x4f. All five names are in the table, so the server sent no name strings.

### DATA frame

```
header   00 00 14 00 01 00 00 01
payload  48 65 6c 6c 6f 2c 20 62  69 6e 61 72 79 20 48 54
         54 50 21 0a
```

| bytes | field | meaning |
|---|---|---|
| `00 00 14` | Length | 0x14 = 20 |
| `00` | Type | DATA |
| `01` | Flags | END_STREAM: this is the last frame of the response |
| `00 00 01` | Stream ID | 1 |
| `48 65 … 21 0a` | payload | `Hello, binary HTTP!\n`, written to stdout as it is |

After END_STREAM the client could have sent stream 2 on the same socket. It had nothing else to fetch, so it closed the connection and exited with 0.

## Two-byte lengths

Every string above is shorter than 128 bytes, so each length took one byte with the top bit clear. A longer string sets the top bit and uses 15 bits across two bytes. For example, a 300-byte value (300 = 0x012c) is prefixed `81 2c`. The largest one-byte length is `7f` (127). The smallest two-byte one is `80 80` (128). `tests/interop.py` builds an exactly 64 KiB header block from values of up to 32 767 bytes (prefix `ff ff`).

## An unknown frame being skipped

`bcurl --grease` sends this frame on stream 0 before the request:

```
header   00 00 1d fa 00 00 00 00
payload  75 6e 6b 6e 6f 77 6e 20  66 72 61 6d 65 2c 20 70   unknown frame, p
         6c 65 61 73 65 20 73 6b  69 70 20 6d 65            lease skip me
```

The header gives Length 0x1d = 29, Type `fa`, which is in the never-assigned range `f0`–`ff`, Flags 0 and stream 0. The server does not know type `fa`, so it reads the 29 payload bytes, discards them and goes on to the HEADERS frame behind it. The response is the same as above. `tests/interop.py` checks the same thing in both directions, with unknown frames before a request, between DATA frames of a response and inside a request body.

## Raw `-v` output

Unedited stderr from the command at the top (`>` sent, `<` received):

```
* connected to localhost port 9000 (BHTTP/1)
> HEADERS frame  length=66 type=0x01 flags=0x01 [END_STREAM] stream=1
>   header  00 00 42 | 01 | 01 | 00 00 01    (length | type | flags | stream)
>   000000  01 03 47 45 54 02 0a 2f  68 65 6c 6c 6f 2e 74 78  |..GET../hello.tx|
>   000010  74 04 0e 6c 6f 63 61 6c  68 6f 73 74 3a 39 30 30  |t..localhost:900|
>   000020  30 05 09 62 63 75 72 6c  2f 31 2e 30 06 03 2a 2f  |0..bcurl/1.0..*/|
>   000030  2a 00 0a 78 2d 74 72 61  63 65 2d 69 64 04 37 66  |*..x-trace-id.7f|
>   000040  33 61                                             |3a|
>     [#1 ] :method: GET
>     [#2 ] :path: /hello.txt
>     [#4 ] host: localhost:9000
>     [#5 ] user-agent: bcurl/1.0
>     [#6 ] accept: */*
>     [lit] x-trace-id: 7f3a
< HEADERS frame  length=79 type=0x01 flags=0x00 stream=1
<   header  00 00 4f | 01 | 00 | 00 00 01    (length | type | flags | stream)
<   000000  03 03 32 30 30 07 0a 62  73 65 72 76 65 2f 31 2e  |..200..bserve/1.|
<   000010  30 08 1d 54 75 65 2c 20  30 36 20 4f 63 74 20 32  |0..Tue, 06 Oct 2|
<   000020  30 32 36 20 32 30 3a 34  32 3a 35 38 20 47 4d 54  |026 20:42:58 GMT|
<   000030  09 19 74 65 78 74 2f 70  6c 61 69 6e 3b 20 63 68  |..text/plain; ch|
<   000040  61 72 73 65 74 3d 75 74  66 2d 38 0a 02 32 30     |arset=utf-8..20|
<     [#3 ] :status: 200
<     [#7 ] server: bserve/1.0
<     [#8 ] date: Tue, 06 Oct 2026 20:42:58 GMT
<     [#9 ] content-type: text/plain; charset=utf-8
<     [#10] content-length: 20
< DATA frame  length=20 type=0x00 flags=0x01 [END_STREAM] stream=1
<   header  00 00 14 | 00 | 01 | 00 00 01    (length | type | flags | stream)
<   000000  48 65 6c 6c 6f 2c 20 62  69 6e 61 72 79 20 48 54  |Hello, binary HT|
<   000010  54 50 21 0a                                       |TP!.|
* stream 1: GET /hello.txt -> 200
* connection closed, exit 0
```
