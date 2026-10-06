# Annotated hexdump of one request and response

This is one exchange between `./bcurl` and `./bserve ./www 9000`. The request asks for `/hello.txt`, a 20-byte file containing `Hello, binary HTTP!\n`. I added one header that is not in the static table (`-H 'x-trace-id: 7f3a'`) so the dump also shows a literal name:

```
./bcurl -v -H 'x-trace-id: 7f3a' localhost:9000/hello.txt
```

On the wire there are three frames and 189 bytes: a 74-byte request and a 115-byte response.

Offsets in the tables count from the first payload byte, starting at 0, as in the `-v` output, so payload offset 0x00 is frame byte 8 (also counted from 0). The `-v` output of the first two captures is included unedited; the third is cut after the request.

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
| `48 65 … 21 0a` | payload | `Hello, binary HTTP!\n`, written to stdout unchanged |

After END_STREAM the client could have sent stream 2 on the same socket. It had nothing else to fetch, so it closed the connection and exited with 0.

## A 404 with a two-byte length

Every string above is shorter than 128 bytes, so each length took one byte with the top bit clear. Longer strings set the top bit and use 15 bits across two bytes: 127 is `7f`, 128 is `80 80`, 300 is `81 2c` and the largest, 32 767, is `ff ff`. Asking for a file that does not exist, with a 140-byte name, shows the two-byte form and an error response in one exchange:

```
./bcurl -v localhost:9000/notes/2026/network-architecture/week-6/interop-results-between-our-bserve-and-bcurl-and-the-independent-python-peer-in-the-tests-folder.txt
```

| where | bytes | decoded |
|---|---|---|
| request header | `00 00 b4 01 01 00 00 01` | Length 0xb4 = 180, HEADERS, END_STREAM, stream 1 |
| payload 0x05 | `02` | tag 2 `:path` |
| payload 0x06 | `80 8c` | top bit set, so two bytes: 0x008c = 140 |
| payload 0x08 | `2f 6e 6f 74 65 73 …` | the 140-byte path, ending at 0x93 |
| request total | | 5 + (1 + 2 + 140) + 16 + 11 + 5 = 180 = 0xb4 |
| response header | `00 00 50 01 00 00 00 01` | Length 0x50 = 80, HEADERS, Flags 0 (a body follows), stream 1 |
| payload 0x00 | `03 03 34 30 34` | tag 3 `:status`, length 3, `404` |
| payload 0x05 | `07 0a 62 73 … 2e 30` | tag 7 `server`, length 10, `bserve/1.0` |
| payload 0x11 | `08 1d 54 75 … 4d 54` | tag 8 `date`, length 29 |
| payload 0x30 | `09 19 74 65 … 2d 38` | tag 9 `content-type`, length 25, `text/plain; charset=utf-8` |
| payload 0x4b | `0a 03 31 35 35` | tag 10 `content-length`, length 3, `155` (ends at 0x4f, the 80th byte) |
| response DATA | `00 00 9b 00 01 00 00 01` | Length 0x9b = 155, DATA, END_STREAM, stream 1: `404 Not Found\n` plus the path and a newline (14 + 140 + 1) |

bcurl wrote the 155-byte body to stdout and exited with 4 because the status was 4xx. The connection was still usable: a second path on the same command line would have gone out as stream 2.

```
* connected to localhost port 9000 (BHTTP/1)
> HEADERS frame  length=180 type=0x01 flags=0x01 [END_STREAM] stream=1
>   header  00 00 b4 | 01 | 01 | 00 00 01    (length | type | flags | stream)
>   000000  01 03 47 45 54 02 80 8c  2f 6e 6f 74 65 73 2f 32  |..GET.../notes/2|
>   000010  30 32 36 2f 6e 65 74 77  6f 72 6b 2d 61 72 63 68  |026/network-arch|
>   000020  69 74 65 63 74 75 72 65  2f 77 65 65 6b 2d 36 2f  |itecture/week-6/|
>   000030  69 6e 74 65 72 6f 70 2d  72 65 73 75 6c 74 73 2d  |interop-results-|
>   000040  62 65 74 77 65 65 6e 2d  6f 75 72 2d 62 73 65 72  |between-our-bser|
>   000050  76 65 2d 61 6e 64 2d 62  63 75 72 6c 2d 61 6e 64  |ve-and-bcurl-and|
>   000060  2d 74 68 65 2d 69 6e 64  65 70 65 6e 64 65 6e 74  |-the-independent|
>   000070  2d 70 79 74 68 6f 6e 2d  70 65 65 72 2d 69 6e 2d  |-python-peer-in-|
>   000080  74 68 65 2d 74 65 73 74  73 2d 66 6f 6c 64 65 72  |the-tests-folder|
>   000090  2e 74 78 74 04 0e 6c 6f  63 61 6c 68 6f 73 74 3a  |.txt..localhost:|
>   0000a0  39 30 30 30 05 09 62 63  75 72 6c 2f 31 2e 30 06  |9000..bcurl/1.0.|
>   0000b0  03 2a 2f 2a                                       |.*/*|
>     [#1 ] :method: GET
>     [#2 ] :path: /notes/2026/network-architecture/week-6/interop-results-between-our-bserve-and-bcurl-and-the-independent-python-peer-in-the-tests-folder.txt
>     [#4 ] host: localhost:9000
>     [#5 ] user-agent: bcurl/1.0
>     [#6 ] accept: */*
< HEADERS frame  length=80 type=0x01 flags=0x00 stream=1
<   header  00 00 50 | 01 | 00 | 00 00 01    (length | type | flags | stream)
<   000000  03 03 34 30 34 07 0a 62  73 65 72 76 65 2f 31 2e  |..404..bserve/1.|
<   000010  30 08 1d 54 75 65 2c 20  30 36 20 4f 63 74 20 32  |0..Tue, 06 Oct 2|
<   000020  30 32 36 20 32 31 3a 30  34 3a 33 35 20 47 4d 54  |026 21:04:35 GMT|
<   000030  09 19 74 65 78 74 2f 70  6c 61 69 6e 3b 20 63 68  |..text/plain; ch|
<   000040  61 72 73 65 74 3d 75 74  66 2d 38 0a 03 31 35 35  |arset=utf-8..155|
<     [#3 ] :status: 404
<     [#7 ] server: bserve/1.0
<     [#8 ] date: Tue, 06 Oct 2026 21:04:35 GMT
<     [#9 ] content-type: text/plain; charset=utf-8
<     [#10] content-length: 155
< DATA frame  length=155 type=0x00 flags=0x01 [END_STREAM] stream=1
<   header  00 00 9b | 00 | 01 | 00 00 01    (length | type | flags | stream)
<   000000  34 30 34 20 4e 6f 74 20  46 6f 75 6e 64 0a 2f 6e  |404 Not Found./n|
<   000010  6f 74 65 73 2f 32 30 32  36 2f 6e 65 74 77 6f 72  |otes/2026/networ|
<   000020  6b 2d 61 72 63 68 69 74  65 63 74 75 72 65 2f 77  |k-architecture/w|
<   000030  65 65 6b 2d 36 2f 69 6e  74 65 72 6f 70 2d 72 65  |eek-6/interop-re|
<   000040  73 75 6c 74 73 2d 62 65  74 77 65 65 6e 2d 6f 75  |sults-between-ou|
<   000050  72 2d 62 73 65 72 76 65  2d 61 6e 64 2d 62 63 75  |r-bserve-and-bcu|
<   000060  72 6c 2d 61 6e 64 2d 74  68 65 2d 69 6e 64 65 70  |rl-and-the-indep|
<   000070  65 6e 64 65 6e 74 2d 70  79 74 68 6f 6e 2d 70 65  |endent-python-pe|
<   000080  65 72 2d 69 6e 2d 74 68  65 2d 74 65 73 74 73 2d  |er-in-the-tests-|
<   000090  66 6f 6c 64 65 72 2e 74  78 74 0a                 |folder.txt.|
* stream 1: GET /notes/2026/network-architecture/week-6/interop-results-between-our-bserve-and-bcurl-and-the-independent-python-peer-in-the-tests-folder.txt -> 404
* connection closed, exit 4
```

## An unknown frame being skipped

`bcurl --grease` sends a frame of type `fa`, from the never-assigned range `f0`–`ff`, on stream 0 before each request. The start of `./bcurl -v --grease localhost:9000/hello.txt`:

```
* connected to localhost port 9000 (BHTTP/1)
> UNKNOWN frame  length=29 type=0xfa flags=0x00 stream=0
>   header  00 00 1d | fa | 00 | 00 00 00    (length | type | flags | stream)
>   000000  75 6e 6b 6e 6f 77 6e 20  66 72 61 6d 65 2c 20 70  |unknown frame, p|
>   000010  6c 65 61 73 65 20 73 6b  69 70 20 6d 65           |lease skip me|
> HEADERS frame  length=49 type=0x01 flags=0x01 [END_STREAM] stream=1
>   header  00 00 31 | 01 | 01 | 00 00 01    (length | type | flags | stream)
>   000000  01 03 47 45 54 02 0a 2f  68 65 6c 6c 6f 2e 74 78  |..GET../hello.tx|
>   000010  74 04 0e 6c 6f 63 61 6c  68 6f 73 74 3a 39 30 30  |t..localhost:900|
>   000020  30 05 09 62 63 75 72 6c  2f 31 2e 30 06 03 2a 2f  |0..bcurl/1.0..*/|
>   000030  2a                                                |*|
>     [#1 ] :method: GET
>     [#2 ] :path: /hello.txt
>     [#4 ] host: localhost:9000
```

The header gives Length 0x1d = 29, Type `fa`, Flags 0 and stream 0. bserve does not know type `fa`, so it reads the 29 payload bytes, discards them and goes on to the HEADERS frame behind it. bserve then answers the request with a HEADERS and a DATA frame laid out exactly like the first exchange's. `tests/interop.py` checks the same rule in both directions: unknown frames before a request, inside a request body and between the DATA frames of a response.

## Raw `-v` output of the first exchange

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
