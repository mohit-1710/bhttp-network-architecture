/*
 * bcurl - BHTTP/1 client (Track 2).
 *
 *   usage: bcurl [-v] [-I] [-X METHOD] [-H 'name: value']... [--grease]
 *                [bhttp://]host[:port][/path] [/more/paths ...]
 *
 * Builds binary request frames, writes response bodies to stdout, and with -v
 * hexdumps every frame (sent '>' and received '<') to stderr. Extra paths are
 * fetched over the SAME connection - bcurl never opens a second one.
 *
 * Exit status: 0 all responses 1xx-3xx, 1 usage, 2 cannot connect,
 *              3 protocol error, 4 a 4xx response, 5 a 5xx response.
 */
#include "bproto.h"

#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_PORT "9000"
#define USER_AGENT   "bcurl/1.0"
#define MAX_EXTRA    32
#define GREASE_TYPE  0xFA   /* deliberately unassigned frame type */

enum { EX_OK = 0, EX_USAGE = 1, EX_CONNECT = 2, EX_PROTO = 3, EX_4XX = 4, EX_5XX = 5 };

static FILE *g_trace;

static void usage(void)
{
    fprintf(stderr,
            "usage: bcurl [-v] [-I] [-X METHOD] [-H 'name: value']... [--grease]\n"
            "             [bhttp://]host[:port][/path] [/more/paths ...]\n");
    exit(EX_USAGE);
}

typedef struct { char host[256]; char port[8]; char path[4096]; } target;

/* Parse [bhttp://]host[:port][/path], with [v6]:port allowed. */
static int parse_url(const char *url, target *t)
{
    if (strncmp(url, "bhttp://", 8) == 0)
        url += 8;
    const char *slash = strchr(url, '/');
    size_t alen = slash ? (size_t)(slash - url) : strlen(url);
    char auth[300];
    if (alen == 0 || alen >= sizeof auth)
        return -1;
    memcpy(auth, url, alen);
    auth[alen] = '\0';

    char *host = auth, *port = NULL;
    if (auth[0] == '[') {
        char *rb = strchr(auth, ']');
        if (!rb)
            return -1;
        *rb = '\0';
        host = auth + 1;
        if (rb[1] == ':')
            port = rb + 2;
        else if (rb[1])
            return -1;
    } else if ((port = strrchr(auth, ':')) != NULL) {
        *port++ = '\0';
    }
    if (!*host || strlen(host) >= sizeof t->host)
        return -1;
    strcpy(t->host, host);
    if (port && *port) {
        char *end;
        long p = strtol(port, &end, 10);
        if (*end || p < 1 || p > 65535)
            return -1;
        snprintf(t->port, sizeof t->port, "%ld", p);
    } else {
        strcpy(t->port, DEFAULT_PORT);
    }
    const char *path = slash ? slash : "/";
    if (strlen(path) >= sizeof t->path)
        return -1;
    strcpy(t->path, path);
    return 0;
}

static int connect_to(const target *t)
{
    struct addrinfo hints = { 0 }, *res, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(t->host, t->port, &hints, &res);
    if (e) {
        fprintf(stderr, "bcurl: %s: %s\n", t->host, gai_strerror(e));
        return -1;
    }
    int fd = -1;
    /* Try each address until one connects; exactly one connection survives. */
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "bcurl: cannot connect to %s port %s\n", t->host, t->port);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

typedef struct {
    const char *method;
    const char *extra[MAX_EXTRA];   /* "name: value" */
    int nextra;
    int grease;
} options;

static int send_request(int fd, uint32_t sid, const target *t, const options *o)
{
    if (o->grease) {
        static const char junk[] = "unknown frame, please skip me";
        if (bh_send_frame(fd, GREASE_TYPE, 0, 0, junk, sizeof junk - 1, g_trace) != 0)
            return -1;
    }
    char authority[300];
    if (strchr(t->host, ':'))
        snprintf(authority, sizeof authority, "[%s]:%s", t->host, t->port);
    else
        snprintf(authority, sizeof authority, "%s:%s", t->host, t->port);

    bh_buf b = { 0 };
    bh_hb_add(&b, ":method", o->method);
    bh_hb_add(&b, ":path", t->path);
    bh_hb_add(&b, "host", authority);
    bh_hb_add(&b, "user-agent", USER_AGENT);
    bh_hb_add(&b, "accept", "*/*");
    for (int i = 0; i < o->nextra; i++) {
        char name[256];
        const char *colon = strchr(o->extra[i], ':');
        size_t nl = colon ? (size_t)(colon - o->extra[i]) : 0;
        if (!colon || nl == 0 || nl >= sizeof name) {
            fprintf(stderr, "bcurl: bad header '%s'\n", o->extra[i]);
            bh_buf_free(&b);
            exit(EX_USAGE);
        }
        for (size_t k = 0; k < nl; k++)
            name[k] = (char)tolower((unsigned char)o->extra[i][k]);
        name[nl] = '\0';
        const char *val = colon + 1;
        while (*val == ' ')
            val++;
        bh_hb_add(&b, name, val);
    }
    int rc = b.err ? -1
                   : bh_send_frame(fd, BH_HEADERS, BH_FLAG_END_STREAM, sid, b.buf,
                                   (uint32_t)b.len, g_trace);
    bh_buf_free(&b);
    return rc;
}

/* Read one response on stream sid. Returns the status code, or -1. */
static int read_response(int fd, uint32_t sid)
{
    bh_frame f;
    int r = bh_next_frame(fd, &f, g_trace);
    if (r != 0) {
        fprintf(stderr, "bcurl: connection closed before a response arrived\n");
        return -1;
    }
    if (f.type != BH_HEADERS || f.stream != sid) {
        fprintf(stderr, "bcurl: expected HEADERS on stream %u, got type 0x%02x on stream %u\n",
                sid, f.type, f.stream);
        return -1;
    }
    if (f.length > 1u << 20) {
        fprintf(stderr, "bcurl: response header block too large (%u bytes)\n", f.length);
        return -1;
    }
    uint8_t *block = malloc(f.length ? f.length : 1);
    if (!block || bh_read_payload(fd, &f, block, g_trace) != 0) {
        free(block);
        fprintf(stderr, "bcurl: truncated HEADERS frame\n");
        return -1;
    }
    if (g_trace)
        bh_trace_fields(g_trace, '<', block, f.length);

    bh_headers h;
    int status = -1;
    if (bh_hb_decode(block, f.length, &h) == 0) {
        const char *s = bh_get(&h, ":status");
        char *end;
        long v = s ? strtol(s, &end, 10) : -1;
        if (s && !*end && v >= 100 && v <= 599 && bh_count(&h, ":status") == 1)
            status = (int)v;
        bh_headers_free(&h);
    }
    free(block);
    if (status < 0) {
        fprintf(stderr, "bcurl: response has a malformed header block or no :status\n");
        return -1;
    }

    int end = f.flags & BH_FLAG_END_STREAM;
    while (!end) {
        if (bh_next_frame(fd, &f, g_trace) != 0) {
            fprintf(stderr, "bcurl: connection closed mid-response\n");
            return -1;
        }
        if (f.type != BH_DATA || f.stream != sid) {
            fprintf(stderr, "bcurl: expected DATA on stream %u\n", sid);
            return -1;
        }
        /* Stream the payload to stdout without buffering the whole body. */
        uint8_t chunk[BH_DATA_CHUNK];
        size_t done = 0;
        while (done < f.length) {
            size_t n = f.length - done;
            if (n > sizeof chunk)
                n = sizeof chunk;
            if (bh_read_full(fd, chunk, n) != 0) {
                fprintf(stderr, "bcurl: truncated DATA frame\n");
                return -1;
            }
            if (g_trace)
                bh_hexdump(g_trace, '<', chunk, n, done);
            fwrite(chunk, 1, n, stdout);
            done += n;
        }
        end = f.flags & BH_FLAG_END_STREAM;
    }
    fflush(stdout);
    return status;
}

int main(int argc, char **argv)
{
    options o = { .method = "GET" };
    const char *urls[256];
    int nurls = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-v") == 0)
            g_trace = stderr;
        else if (strcmp(a, "-I") == 0)
            o.method = "HEAD";
        else if (strcmp(a, "-X") == 0 && i + 1 < argc)
            o.method = argv[++i];
        else if (strcmp(a, "-H") == 0 && i + 1 < argc && o.nextra < MAX_EXTRA)
            o.extra[o.nextra++] = argv[++i];
        else if (strcmp(a, "--grease") == 0)
            o.grease = 1;
        else if (a[0] == '-')
            usage();
        else if (nurls < (int)(sizeof urls / sizeof urls[0]))
            urls[nurls++] = a;
    }
    if (nurls == 0)
        usage();

    target base;
    if (parse_url(urls[0], &base) != 0) {
        fprintf(stderr, "bcurl: bad URL '%s'\n", urls[0]);
        return EX_USAGE;
    }
    /* Every later argument must address the same server: one connection only. */
    target ts[256];
    ts[0] = base;
    for (int i = 1; i < nurls; i++) {
        ts[i] = base;
        if (urls[i][0] == '/') {
            if (strlen(urls[i]) >= sizeof ts[i].path)
                return EX_USAGE;
            strcpy(ts[i].path, urls[i]);
        } else if (parse_url(urls[i], &ts[i]) != 0 || strcmp(ts[i].host, base.host) != 0 ||
                   strcmp(ts[i].port, base.port) != 0) {
            fprintf(stderr, "bcurl: '%s' is not on %s:%s; bcurl never opens a second connection\n",
                    urls[i], base.host, base.port);
            return EX_USAGE;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    int fd = connect_to(&base);
    if (fd < 0)
        return EX_CONNECT;
    if (g_trace)
        fprintf(g_trace, "* connected to %s port %s (BHTTP/1)\n", base.host, base.port);

    int rc = EX_OK;
    for (int i = 0; i < nurls; i++) {
        uint32_t sid = (uint32_t)i + 1;
        if (send_request(fd, sid, &ts[i], &o) != 0) {
            fprintf(stderr, "bcurl: failed to send request\n");
            rc = EX_PROTO;
            break;
        }
        int status = read_response(fd, sid);
        if (status < 0) {
            rc = EX_PROTO;
            break;
        }
        if (g_trace)
            fprintf(g_trace, "* stream %u: %s %s -> %d\n", sid, o.method, ts[i].path, status);
        if (status >= 500)
            rc = EX_5XX;
        else if (status >= 400 && rc != EX_5XX)
            rc = EX_4XX;
    }
    close(fd);
    if (g_trace)
        fprintf(g_trace, "* connection closed, exit %d\n", rc);
    return rc;
}
