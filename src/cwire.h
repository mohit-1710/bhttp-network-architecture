/*
 * cwire.h - bcurl's own BHTTP/1 wire code, written from SPEC.md.
 *
 * bserve uses bproto.[ch]; bcurl uses only this file, so the two programs
 * share the spec and nothing else. A misreading in one codec cannot hide
 * behind the same misreading in the other.
 */
#ifndef CWIRE_H
#define CWIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* SPEC §2-§5 */
#define CW_HEADER_LEN       8
#define CW_DATA             0x00
#define CW_HEADERS          0x01
#define CW_END_STREAM       0x01
#define CW_GREASE_TYPE      0xFA        /* from the never-assigned 0xF0-0xFF range */
#define CW_MAX_LENGTH       0xFFFFFFu
#define CW_MAX_STREAM       0xFFFFFFu
#define CW_MAX_HEADER_BLOCK 65535u
#define CW_MAX_STRING       0x7FFFu
#define CW_MAX_PATH         1024u
#define CW_CHUNK            16384u

typedef struct {
    uint32_t length, stream;
    uint8_t  type, flags;
} cw_frame;

/* Reads give up with ETIMEDOUT once the deadline (seconds from now) passes. */
void cw_set_deadline(int seconds);
int  cw_read_full(int fd, void *buf, size_t n);   /* 0 ok, 1 EOF before any byte, -1 error */
int  cw_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                   const void *payload, uint32_t len, FILE *trace);
int  cw_next_frame(int fd, cw_frame *f, FILE *trace);   /* skips unknown types (SPEC §3) */
int  cw_read_payload(int fd, const cw_frame *f, uint8_t *buf, FILE *trace);

/* Header block writer. */
typedef struct { uint8_t *data; size_t len, cap; int failed; } cw_buf;
void cw_hb_add(cw_buf *b, const char *name, const char *value);
void cw_buf_free(cw_buf *b);

/* Header block reader. */
typedef struct { char *name; char *value; int tag; } cw_field;
typedef struct { cw_field *fields; int n; } cw_headers;
int         cw_hb_decode(const uint8_t *p, size_t len, cw_headers *h);   /* 0 ok, -1 malformed */
const char *cw_get(const cw_headers *h, const char *name);
void        cw_headers_free(cw_headers *h);

/* Validation and parsing helpers for the command line. */
int  cw_valid_name(const char *name);            /* lowercase token: a literal header name */
int  cw_valid_name_any_case(const char *name);   /* any token: a method */
int  cw_valid_value(const char *value);
int  cw_parse_seconds(const char *s);            /* "1".."3600", else 0 */

/* -v output. */
void        cw_hexdump(FILE *o, char dir, const uint8_t *p, size_t n, size_t base);
void        cw_trace_fields(FILE *o, char dir, const uint8_t *p, size_t len);
const char *cw_escape(const char *s, char *out, size_t n);

#endif
