/* bproto.c - framing, header-block codec and hexdump tracing for BHTTP/1. */
#include "bproto.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* Static table: the ten names bserve and bcurl send. Index 0 = literal name. */
const char *const bh_static_names[BH_STATIC_COUNT + 1] = {
    NULL,
    ":method",        /* 1  */
    ":path",          /* 2  */
    ":status",        /* 3  */
    "host",           /* 4  */
    "user-agent",     /* 5  */
    "accept",         /* 6  */
    "server",         /* 7  */
    "date",           /* 8  */
    "content-type",   /* 9  */
    "content-length", /* 10 */
};

/* ---- frame header --------------------------------------------------- */

void bh_pack_header(uint8_t o[BH_FRAME_HEADER_LEN], const bh_frame *f)
{
    o[0] = (uint8_t)(f->length >> 16);
    o[1] = (uint8_t)(f->length >> 8);
    o[2] = (uint8_t)(f->length);
    o[3] = f->type;
    o[4] = f->flags;
    o[5] = (uint8_t)(f->stream >> 16);
    o[6] = (uint8_t)(f->stream >> 8);
    o[7] = (uint8_t)(f->stream);
}

void bh_unpack_header(const uint8_t i[BH_FRAME_HEADER_LEN], bh_frame *f)
{
    f->length = (uint32_t)i[0] << 16 | (uint32_t)i[1] << 8 | i[2];
    f->type   = i[3];
    f->flags  = i[4];
    f->stream = (uint32_t)i[5] << 16 | (uint32_t)i[6] << 8 | i[7];
}

/* ---- I/O ------------------------------------------------------------ */

static long long g_deadline_ms;     /* monotonic ms; 0 = none */
static int g_frame_timeout;         /* s one bh_send_frame may take; 0 = none */

long long bh_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void bh_set_deadline(int seconds)
{
    g_deadline_ms = seconds > 0 ? bh_now_ms() + (long long)seconds * 1000 : 0;
}

void bh_set_frame_timeout(int seconds)
{
    g_frame_timeout = seconds;
}

/* Wait until fd is readable or the deadline passes. A slow sender cannot
 * stretch one read past the deadline by trickling bytes. */
static int wait_readable(int fd)
{
    if (!g_deadline_ms)
        return 0;
    for (;;) {
        long long left = g_deadline_ms - bh_now_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, left > 60000 ? 60000 : (int)left);
        if (r > 0)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
    }
}

int bh_read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (wait_readable(fd) != 0)
            return -1;
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0)
            return got == 0 ? 1 : -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

int bh_send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream,
                  const void *payload, uint32_t len, FILE *trace)
{
    if (len > BH_MAX_LENGTH || stream > BH_MAX_STREAM)
        return -1;
    uint8_t hdr[BH_FRAME_HEADER_LEN];
    bh_frame f = { len, type, flags, stream };
    bh_pack_header(hdr, &f);
    if (trace) {
        bh_trace_header(trace, '>', &f);
        bh_hexdump(trace, '>', payload, len, 0);
        if (type == BH_HEADERS)
            bh_trace_fields(trace, '>', payload, len);
    }
    /* Header and payload go out in one writev, so they share a segment. */
    struct iovec iov[2] = {
        { hdr, sizeof hdr },
        { (void *)payload, len },
    };
    int cnt = len ? 2 : 1;
    struct iovec *v = iov;
    long long deadline = g_frame_timeout ? bh_now_ms() + g_frame_timeout * 1000LL : 0;
    while (cnt > 0) {
        ssize_t w = writev(fd, v, cnt);
        if (w < 0) {
            /* EAGAIN here means SO_SNDTIMEO passed with nothing written. */
            int e = errno;
            int again = e == EAGAIN || e == EWOULDBLOCK;
            if (e == EINTR || (again && deadline && bh_now_ms() < deadline))
                continue;
            errno = again ? ETIMEDOUT : e;  /* ETIMEDOUT: peer reads too slowly */
            return -1;
        }
        if (deadline && cnt > 0 && bh_now_ms() >= deadline &&
            (size_t)w < v[0].iov_len + (cnt > 1 ? v[1].iov_len : 0)) {
            errno = ETIMEDOUT;          /* peer is reading too slowly */
            return -1;
        }
        while (cnt > 0 && (size_t)w >= v->iov_len) {
            w -= (ssize_t)v->iov_len;
            v++;
            cnt--;
        }
        if (cnt > 0) {
            v->iov_base = (char *)v->iov_base + w;
            v->iov_len -= (size_t)w;
        }
    }
    return 0;
}

int bh_read_payload(int fd, const bh_frame *f, uint8_t *buf, FILE *trace)
{
    uint8_t tmp[BH_DATA_CHUNK];
    size_t done = 0;
    while (done < f->length) {
        size_t n = f->length - done;
        if (n > sizeof tmp)
            n = sizeof tmp;
        uint8_t *dst = buf ? buf + done : tmp;
        if (bh_read_full(fd, dst, n) != 0)
            return -1;
        if (trace)
            bh_hexdump(trace, '<', dst, n, done);
        done += n;
    }
    return 0;
}

int bh_next_frame(int fd, bh_frame *f, FILE *trace)
{
    for (;;) {
        uint8_t hdr[BH_FRAME_HEADER_LEN];
        int r = bh_read_full(fd, hdr, sizeof hdr);
        if (r != 0)
            return r;
        bh_unpack_header(hdr, f);
        if (trace)
            bh_trace_header(trace, '<', f);
        if (f->type == BH_DATA || f->type == BH_HEADERS)
            return 0;
        /* Unknown type: Length says exactly how far to skip (SPEC §3).
         * Its flags and stream id are ignored. */
        if (bh_read_payload(fd, f, NULL, trace) != 0)
            return -1;
        if (trace)
            fprintf(trace, "< (unknown frame type 0x%02x skipped)\n", f->type);
    }
}

/* ---- header block codec --------------------------------------------- */
/*
 * field  := tag(1) [string]{name, only if tag == 0} string{value}
 * string := len value-bytes
 * len    := 0xxxxxxx                    (0..127, one byte)
 *         | 1xxxxxxx xxxxxxxx           (0..32767, two bytes, big-endian)
 */

static void buf_put(bh_buf *b, const void *p, size_t n)
{
    if (b->err)
        return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *nb = realloc(b->buf, cap);
        if (!nb) {
            b->err = 1;
            return;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

static void put_string(bh_buf *b, const char *s)
{
    size_t n = strlen(s);
    if (n > BH_MAX_STRING) {
        b->err = 1;
        return;
    }
    uint8_t pre[2];
    if (n < 0x80) {
        pre[0] = (uint8_t)n;
        buf_put(b, pre, 1);
    } else {
        pre[0] = (uint8_t)(0x80 | (n >> 8));
        pre[1] = (uint8_t)n;
        buf_put(b, pre, 2);
    }
    buf_put(b, s, n);
}

void bh_hb_add(bh_buf *b, const char *name, const char *value)
{
    uint8_t tag = 0;
    for (int i = 1; i <= BH_STATIC_COUNT; i++)
        if (strcmp(name, bh_static_names[i]) == 0)
            tag = (uint8_t)i;
    buf_put(b, &tag, 1);
    if (tag == 0)
        put_string(b, name);
    put_string(b, value);
}

void bh_buf_free(bh_buf *b)
{
    free(b->buf);
    memset(b, 0, sizeof *b);
}

static int get_len(const uint8_t *p, size_t len, size_t *pos, size_t *out)
{
    if (*pos >= len)
        return -1;
    uint8_t b = p[(*pos)++];
    if (b < 0x80) {
        *out = b;
        return 0;
    }
    if (*pos >= len)
        return -1;
    *out = (size_t)(b & 0x7F) << 8 | p[(*pos)++];
    return 0;
}

/* Literal names: HTTP token characters (RFC 9110 §5.6.2), lowercase only.
 * ':' is not a token character, so pseudo-headers can only use the table. */
static int name_ok(const uint8_t *s, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              (c && strchr("!#$%&'*+-.^_`|~", c))))
            return 0;
    }
    return 1;
}

static int value_ok(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] == 0 || s[i] == '\r' || s[i] == '\n')
            return 0;
    return 1;
}

int bh_valid_name(const char *name)
{
    return name_ok((const uint8_t *)name, strlen(name)) && strlen(name) <= BH_MAX_STRING;
}

int bh_valid_name_any_case(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > BH_MAX_STRING)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              (c && strchr("!#$%&'*+-.^_`|~", c))))
            return 0;
    }
    return 1;
}

int bh_parse_seconds(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    return (*s >= '0' && *s <= '9' && *end == '\0' && v >= 1 && v <= 3600) ? (int)v : 0;
}

int bh_valid_value(const char *value)
{
    return strlen(value) <= BH_MAX_STRING && !strpbrk(value, "\r\n");
}

int bh_hb_decode(const uint8_t *p, size_t len, bh_headers *h)
{
    memset(h, 0, sizeof *h);
    /* A field is at least 2 bytes, so there are at most len/2 of them, and
     * every string copy fits in len bytes plus one NUL per string. */
    size_t max_fields = len / 2 + 1;
    h->f = malloc(max_fields * sizeof *h->f);
    h->arena = malloc(len + 2 * max_fields + 1);
    if (!h->f || !h->arena)
        goto bad;
    size_t pos = 0, a = 0, n;
    int seen_regular = 0;

    while (pos < len) {
        bh_field *f = &h->f[h->n];
        uint8_t tag = p[pos++];
        if (tag == 0) {
            if (get_len(p, len, &pos, &n) || n > len - pos || !name_ok(p + pos, n))
                goto bad;
            memcpy(h->arena + a, p + pos, n);
            h->arena[a + n] = '\0';
            f->name = h->arena + a;
            a += n + 1;
            pos += n;
        } else if (tag <= BH_STATIC_COUNT) {
            f->name = bh_static_names[tag];
        } else {
            goto bad;                    /* reserved index */
        }
        f->index = tag;
        /* Pseudo-headers must come before every regular field. */
        if (f->name[0] == ':') {
            if (seen_regular)
                goto bad;
        } else {
            seen_regular = 1;
        }
        if (get_len(p, len, &pos, &n) || n > len - pos || !value_ok(p + pos, n))
            goto bad;
        memcpy(h->arena + a, p + pos, n);
        h->arena[a + n] = '\0';
        f->value = h->arena + a;
        a += n + 1;
        pos += n;
        h->n++;
    }
    /* Fields that must not repeat. */
    static const char *const once[] = { ":method", ":path", ":status", "host", "content-length" };
    for (size_t i = 0; i < sizeof once / sizeof once[0]; i++)
        if (bh_count(h, once[i]) > 1)
            goto bad;
    /* content-length is 1 to 18 digits, nothing else. */
    const char *cl = bh_get(h, "content-length");
    if (cl && (!*cl || strlen(cl) > 18 || strspn(cl, "0123456789") != strlen(cl)))
        goto bad;
    return 0;
bad:
    bh_headers_free(h);
    return -1;
}

const char *bh_get(const bh_headers *h, const char *name)
{
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->f[i].name, name) == 0)
            return h->f[i].value;
    return NULL;
}

int bh_count(const bh_headers *h, const char *name)
{
    int c = 0;
    for (int i = 0; i < h->n; i++)
        if (strcmp(h->f[i].name, name) == 0)
            c++;
    return c;
}

void bh_headers_free(bh_headers *h)
{
    free(h->f);
    free(h->arena);
    h->f = NULL;
    h->arena = NULL;
    h->n = 0;
}

/* ---- tracing -------------------------------------------------------- */

const char *bh_escape(const char *s, char *out, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (; *s && o + 5 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out[o++] = (char)c;
        } else {
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (*s && o + 4 <= n) {           /* truncated */
        memcpy(out + o, "...", 3);
        o += 3;
    }
    out[o] = '\0';
    return out;
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case BH_DATA:    return "DATA";
    case BH_HEADERS: return "HEADERS";
    default:         return "UNKNOWN";
    }
}

void bh_trace_header(FILE *o, char dir, const bh_frame *f)
{
    uint8_t raw[BH_FRAME_HEADER_LEN];
    bh_pack_header(raw, f);
    fprintf(o, "%c %s frame  length=%u type=0x%02x flags=0x%02x%s stream=%u\n",
            dir, type_name(f->type), f->length, f->type, f->flags,
            (f->flags & BH_FLAG_END_STREAM) ? " [END_STREAM]" : "", f->stream);
    fprintf(o, "%c   header  %02x %02x %02x | %02x | %02x | %02x %02x %02x"
               "    (length | type | flags | stream)\n",
            dir, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
    fflush(o);
}

void bh_hexdump(FILE *o, char dir, const uint8_t *p, size_t n, size_t base)
{
    for (size_t off = 0; off < n; off += 16) {
        fprintf(o, "%c   %06zx  ", dir, base + off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < n)
                fprintf(o, "%02x ", p[off + i]);
            else
                fputs("   ", o);
            if (i == 7)
                fputc(' ', o);
        }
        fputs(" |", o);
        for (size_t i = 0; i < 16 && off + i < n; i++) {
            uint8_t c = p[off + i];
            fputc(c >= 0x20 && c < 0x7F ? c : '.', o);
        }
        fputs("|\n", o);
    }
    fflush(o);
}

void bh_trace_fields(FILE *o, char dir, const uint8_t *p, size_t len)
{
    bh_headers h;
    if (bh_hb_decode(p, len, &h) != 0) {
        fprintf(o, "%c   (malformed header block)\n", dir);
        return;
    }
    char nb[256], vb[512];
    for (int i = 0; i < h.n; i++) {
        bh_escape(h.f[i].name, nb, sizeof nb);
        bh_escape(h.f[i].value, vb, sizeof vb);
        if (h.f[i].index)
            fprintf(o, "%c     [#%-2d] %s: %s\n", dir, h.f[i].index, nb, vb);
        else
            fprintf(o, "%c     [lit] %s: %s\n", dir, nb, vb);
    }
    bh_headers_free(&h);
    fflush(o);
}
