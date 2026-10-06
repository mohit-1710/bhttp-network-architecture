/*
 * bproto.h - shared framing for BHTTP/1 (see SPEC.md).
 *
 * Frame header, 8 bytes, network byte order:
 *
 *   +-----------------------------------------------+---------------+
 *   |                 Length (24)                   |   Type (8)    |
 *   +---------------+-----------------------------------------------+
 *   |   Flags (8)   |               Stream ID (24)                  |
 *   +---------------+-----------------------------------------------+
 */
#ifndef BPROTO_H
#define BPROTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BH_FRAME_HEADER_LEN 8
#define BH_MAX_LENGTH       0xFFFFFFu   /* 24-bit length field */
#define BH_MAX_STREAM       0xFFFFFFu   /* 24-bit stream id    */
#define BH_MAX_HEADER_BLOCK 65536u      /* receivers MUST accept at least this */
#define BH_DATA_CHUNK       16384u      /* senders SHOULD NOT exceed this per DATA frame */
#define BH_MAX_STRING       0x7FFFu     /* longest string a 2-byte length prefix holds */
#define BH_MAX_FIELDS       64

/* Frame types. Anything else is unknown and MUST be skipped. */
enum { BH_DATA = 0x00, BH_HEADERS = 0x01 };

/* Flags. Unknown flag bits MUST be ignored by receivers. */
#define BH_FLAG_END_STREAM 0x01

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint8_t  flags;
    uint32_t stream;
} bh_frame;

void bh_pack_header(uint8_t out[BH_FRAME_HEADER_LEN], const bh_frame *f);
void bh_unpack_header(const uint8_t in[BH_FRAME_HEADER_LEN], bh_frame *f);

/* Blocking I/O. read_full: 0 ok, 1 clean EOF before any byte, -1 error/short read. */
int bh_read_full(int fd, void *buf, size_t n);
int bh_write_full(int fd, const void *buf, size_t n);

/* Send one frame. If trace != NULL, hexdump it there with the given direction marker. */
int bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                  const void *payload, uint32_t len, FILE *trace);

/* Read the next frame header whose type is known (DATA/HEADERS), skipping and
 * (optionally) tracing the payload of every unknown frame on the way.
 * Returns 0 ok, 1 clean EOF, -1 error. */
int bh_next_frame(int fd, bh_frame *f, FILE *trace);

/* Read (or discard, if buf == NULL) a payload of f->length bytes, tracing it. */
int bh_read_payload(int fd, const bh_frame *f, uint8_t *buf, FILE *trace);

/* ---- header blocks -------------------------------------------------- */

#define BH_STATIC_COUNT 10
extern const char *const bh_static_names[BH_STATIC_COUNT + 1];

typedef struct { uint8_t *buf; size_t len, cap; int err; } bh_buf;
void bh_hb_add(bh_buf *b, const char *name, const char *value);
void bh_buf_free(bh_buf *b);

typedef struct { const char *name; const char *value; int index; } bh_field;
typedef struct { bh_field f[BH_MAX_FIELDS]; int n; char *arena; } bh_headers;

int  bh_hb_decode(const uint8_t *p, size_t len, bh_headers *h); /* 0 ok, -1 malformed */
const char *bh_get(const bh_headers *h, const char *name);
int  bh_count(const bh_headers *h, const char *name);
void bh_headers_free(bh_headers *h);

/* ---- tracing (-v) ---------------------------------------------------- */
void bh_trace_header(FILE *o, char dir, const bh_frame *f);
void bh_hexdump(FILE *o, char dir, const uint8_t *p, size_t n, size_t base);
void bh_trace_fields(FILE *o, char dir, const uint8_t *p, size_t len);

#endif
