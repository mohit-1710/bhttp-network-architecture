/* cwire.c - bcurl's BHTTP/1 codec (see cwire.h). */
#include "cwire.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const table[11] = {
    NULL, ":method", ":path", ":status", "host", "user-agent", "accept",
    "server", "date", "content-type", "content-length",
};

/* ---- reading and writing frames ------------------------------------- */

static long long deadline;          /* CLOCK_MONOTONIC ms, 0 = none */

static long long now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

void cw_set_deadline(int seconds)
{
    deadline = seconds > 0 ? now() + seconds * 1000LL : 0;
}

int cw_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t have = 0;
    while (have < n) {
        if (deadline) {
            long long left = deadline - now();
            struct pollfd pf = { fd, POLLIN, 0 };
            if (left <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            int r = poll(&pf, 1, left > 60000 ? 60000 : (int)left);
            if (r < 0 && errno != EINTR)
                return -1;
            if (r <= 0)
                continue;           /* recheck the clock */
        }
        ssize_t got = read(fd, p + have, n - have);
        if (got == 0)
            return have ? -1 : 1;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        have += (size_t)got;
    }
    return 0;
}

static const char *kind(uint8_t type)
{
    return type == CW_HEADERS ? "HEADERS" : type == CW_DATA ? "DATA" : "UNKNOWN";
}

static void trace_frame(FILE *o, char dir, const cw_frame *f)
{
    fprintf(o, "%c %s frame  length=%u type=0x%02x flags=0x%02x%s stream=%u\n",
            dir, kind(f->type), f->length, f->type, f->flags,
            (f->flags & CW_END_STREAM) ? " [END_STREAM]" : "", f->stream);
    fprintf(o, "%c   header  %02x %02x %02x | %02x | %02x | %02x %02x %02x"
               "    (length | type | flags | stream)\n", dir,
            (f->length >> 16) & 0xff, (f->length >> 8) & 0xff, f->length & 0xff, f->type,
            f->flags, (f->stream >> 16) & 0xff, (f->stream >> 8) & 0xff, f->stream & 0xff);
    fflush(o);
}

int cw_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                  const void *payload, uint32_t len, FILE *trace)
{
    if (len > CW_MAX_LENGTH || stream > CW_MAX_STREAM)
        return -1;
    cw_frame f = { len, stream, type, flags };
    if (trace) {
        trace_frame(trace, '>', &f);
        cw_hexdump(trace, '>', payload, len, 0);
        if (type == CW_HEADERS)
            cw_trace_fields(trace, '>', payload, len);
    }
    /* Header and payload in one buffer, one write loop. */
    size_t total = CW_HEADER_LEN + (size_t)len;
    uint8_t *out = malloc(total);
    if (!out)
        return -1;
    out[0] = (uint8_t)(len >> 16);
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    out[3] = type;
    out[4] = flags;
    out[5] = (uint8_t)(stream >> 16);
    out[6] = (uint8_t)(stream >> 8);
    out[7] = (uint8_t)stream;
    if (len)
        memcpy(out + CW_HEADER_LEN, payload, len);
    size_t sent = 0;
    int rc = 0;
    while (sent < total) {
        ssize_t w = write(fd, out + sent, total - sent);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0) {
            rc = -1;
            break;
        }
        sent += (size_t)w;
    }
    free(out);
    return rc;
}

int cw_read_payload(int fd, const cw_frame *f, uint8_t *buf, FILE *trace)
{
    uint8_t scratch[CW_CHUNK];
    for (size_t off = 0; off < f->length; ) {
        size_t n = f->length - off < sizeof scratch ? f->length - off : sizeof scratch;
        uint8_t *dst = buf ? buf + off : scratch;
        if (cw_read_full(fd, dst, n) != 0)
            return -1;
        if (trace)
            cw_hexdump(trace, '<', dst, n, off);
        off += n;
    }
    return 0;
}

int cw_next_frame(int fd, cw_frame *f, FILE *trace)
{
    for (;;) {
        uint8_t h[CW_HEADER_LEN];
        int r = cw_read_full(fd, h, sizeof h);
        if (r)
            return r;
        f->length = (uint32_t)h[0] << 16 | (uint32_t)h[1] << 8 | h[2];
        f->type = h[3];
        f->flags = h[4];
        f->stream = (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7];
        if (trace)
            trace_frame(trace, '<', f);
        if (f->type == CW_DATA || f->type == CW_HEADERS)
            return 0;
        /* SPEC §3: read past an unknown type by its Length, whatever its
         * stream or flags, and carry on. */
        if (cw_read_payload(fd, f, NULL, trace) != 0)
            return -1;
        if (trace)
            fprintf(trace, "< (unknown frame type 0x%02x skipped)\n", f->type);
    }
}

/* ---- header blocks (SPEC §5) ---------------------------------------- */

static void append(cw_buf *b, const void *p, size_t n)
{
    if (b->failed)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap * 2 + n + 64;
        uint8_t *d = realloc(b->data, cap);
        if (!d) {
            b->failed = 1;
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void append_string(cw_buf *b, const char *s)
{
    size_t n = strlen(s);
    uint8_t pre[2] = { (uint8_t)(0x80 | (n >> 8)), (uint8_t)n };
    if (n > CW_MAX_STRING) {
        b->failed = 1;
        return;
    }
    if (n < 0x80)
        append(b, pre + 1, 1);          /* one byte, top bit clear */
    else
        append(b, pre, 2);
    append(b, s, n);
}

void cw_hb_add(cw_buf *b, const char *name, const char *value)
{
    uint8_t tag = 0;
    for (uint8_t t = 1; t <= 10 && !tag; t++)
        if (strcmp(table[t], name) == 0)
            tag = t;
    append(b, &tag, 1);
    if (!tag)
        append_string(b, name);
    append_string(b, value);
}

void cw_buf_free(cw_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int is_tchar(unsigned char c, int any_case)
{
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    if (any_case && c >= 'A' && c <= 'Z')
        return 1;
    return c && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int token_ok(const char *s, size_t n, int any_case)
{
    if (n == 0 || n > CW_MAX_STRING)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!is_tchar((unsigned char)s[i], any_case))
            return 0;
    return 1;
}

int cw_valid_name(const char *name)          { return token_ok(name, strlen(name), 0); }
int cw_valid_name_any_case(const char *name) { return token_ok(name, strlen(name), 1); }

int cw_valid_value(const char *value)
{
    return strlen(value) <= CW_MAX_STRING && !strpbrk(value, "\r\n");
}

int cw_parse_seconds(const char *s)
{
    if (!*s || strspn(s, "0123456789") != strlen(s) || strlen(s) > 4)
        return 0;
    int v = atoi(s);
    return v >= 1 && v <= 3600 ? v : 0;
}

/* Read one length prefix; returns the length or -1 if it runs off the end. */
static long take_len(const uint8_t *p, size_t len, size_t *at)
{
    if (*at >= len)
        return -1;
    uint8_t first = p[(*at)++];
    if (!(first & 0x80))
        return first;
    if (*at >= len)
        return -1;
    return (long)(first & 0x7f) << 8 | p[(*at)++];
}

static char *take_string(const uint8_t *p, size_t len, size_t *at)
{
    long n = take_len(p, len, at);
    if (n < 0 || (size_t)n > len - *at)
        return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s)
        return NULL;
    memcpy(s, p + *at, (size_t)n);
    s[n] = '\0';
    *at += (size_t)n;
    if (memchr(p + *at - n, 0, (size_t)n) || strpbrk(s, "\r\n")) {
        free(s);                        /* NUL, CR and LF are never allowed */
        return NULL;
    }
    return s;
}

int cw_hb_decode(const uint8_t *p, size_t len, cw_headers *h)
{
    int cap = 0, regular_seen = 0;
    h->fields = NULL;
    h->n = 0;
    for (size_t at = 0; at < len; ) {
        uint8_t tag = p[at++];
        char *name = NULL;
        if (tag == 0) {
            name = take_string(p, len, &at);
            if (!name || !cw_valid_name(name)) {
                free(name);
                goto malformed;
            }
        } else if (tag <= 10) {
            name = strdup(table[tag]);
        } else {
            goto malformed;             /* tags 11-255 are not defined in v1 */
        }
        char *value = name ? take_string(p, len, &at) : NULL;
        if (!value) {
            free(name);
            goto malformed;
        }
        if (h->n == cap) {
            cap = cap ? cap * 2 : 8;
            cw_field *grown = realloc(h->fields, (size_t)cap * sizeof *grown);
            if (!grown) {
                free(name);
                free(value);
                goto malformed;
            }
            h->fields = grown;
        }
        h->fields[h->n].name = name;
        h->fields[h->n].value = value;
        h->fields[h->n].tag = tag;
        h->n++;
        if (name[0] != ':')
            regular_seen = 1;
        else if (regular_seen)
            goto malformed;             /* pseudo-headers come first */
    }
    static const char *const once[] = { ":method", ":path", ":status", "host", "content-length" };
    for (size_t i = 0; i < sizeof once / sizeof *once; i++) {
        int count = 0;
        for (int j = 0; j < h->n; j++)
            count += strcmp(h->fields[j].name, once[i]) == 0;
        if (count > 1)
            goto malformed;
    }
    const char *cl = cw_get(h, "content-length");
    if (cl && (!*cl || strlen(cl) > 18 || strspn(cl, "0123456789") != strlen(cl)))
        goto malformed;
    return 0;
malformed:
    cw_headers_free(h);
    return -1;
}

const char *cw_get(const cw_headers *h, const char *name)
{
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->fields[i].name, name) == 0)
            return h->fields[i].value;
    return NULL;
}

void cw_headers_free(cw_headers *h)
{
    for (int i = 0; i < h->n; i++) {
        free(h->fields[i].name);
        free(h->fields[i].value);
    }
    free(h->fields);
    h->fields = NULL;
    h->n = 0;
}

/* ---- -v output ------------------------------------------------------ */

const char *cw_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    while (*s && o + 5 < n) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 0x20 && c < 0x7f && c != '\\')
            out[o++] = (char)c;
        else
            o += (size_t)snprintf(out + o, n - o, "\\x%02x", c);
    }
    if (*s && o + 4 <= n) {
        memcpy(out + o, "...", 3);
        o += 3;
    }
    out[o] = '\0';
    return out;
}

void cw_hexdump(FILE *o, char dir, const uint8_t *p, size_t n, size_t base)
{
    for (size_t row = 0; row < n; row += 16) {
        fprintf(o, "%c   %06zx  ", dir, base + row);
        for (size_t i = 0; i < 16; i++) {
            if (row + i < n)
                fprintf(o, "%02x ", p[row + i]);
            else
                fputs("   ", o);
            if (i == 7)
                fputc(' ', o);
        }
        fputs(" |", o);
        for (size_t i = 0; i < 16 && row + i < n; i++)
            fputc(p[row + i] >= 0x20 && p[row + i] < 0x7f ? p[row + i] : '.', o);
        fputs("|\n", o);
    }
    fflush(o);
}

void cw_trace_fields(FILE *o, char dir, const uint8_t *p, size_t len)
{
    cw_headers h;
    if (cw_hb_decode(p, len, &h) != 0) {
        fprintf(o, "%c   (malformed header block)\n", dir);
        return;
    }
    char nb[256], vb[512];
    for (int i = 0; i < h.n; i++) {
        cw_escape(h.fields[i].name, nb, sizeof nb);
        cw_escape(h.fields[i].value, vb, sizeof vb);
        if (h.fields[i].tag)
            fprintf(o, "%c     [#%-2d] %s: %s\n", dir, h.fields[i].tag, nb, vb);
        else
            fprintf(o, "%c     [lit] %s: %s\n", dir, nb, vb);
    }
    cw_headers_free(&h);
    fflush(o);
}
