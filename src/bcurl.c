/*
 * bcurl - BHTTP/1 client (Track 2).
 *
 *   usage: bcurl [-v] [-I] [-X METHOD] [-H 'name: value']... [-t seconds] [--grease]
 *                [bhttp://]host[:port][/path] [/more/paths ...]
 *
 * Builds binary request frames, writes response bodies to stdout, and with -v
 * hexdumps every frame (sent '>' and received '<') to stderr. Extra paths are
 * fetched one after another over the same connection.
 *
 * Exit status: 0 all responses 1xx-3xx, 1 usage, 2 cannot connect,
 *              3 protocol, timeout or output error, 4 a 4xx, 5 a 5xx response.
 */
#include "bproto.h"

#include <ctype.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT "9000"
#define USER_AGENT   "bcurl/1.0"
#define MAX_EXTRA    32

enum { EX_OK = 0, EX_USAGE = 1, EX_CONNECT = 2, EX_PROTO = 3, EX_4XX = 4, EX_5XX = 5 };

static FILE *g_trace;
static int   g_timeout = 30;        /* -t: s without any bytes from the server */

static void usage(void)
{
    fprintf(stderr,
            "usage: bcurl [-v] [-I] [-X METHOD] [-H 'name: value']... [-t seconds] [--grease]\n"
            "             [bhttp://]host[:port][/path] [/more/paths ...]\n");
    exit(EX_USAGE);
}

typedef struct { char host[256]; char port[8]; char path[4096]; } target;

/* Parse [bhttp://]host[:port][/path], with [v6]:port allowed. */
static int parse_url(const char *url, target *t)
{
    const char *scheme = strstr(url, "://");
    if (scheme) {
        if (strncmp(url, "bhttp://", 8) != 0)
            return -1;              /* http://, https:// ... are not BHTTP */
        url += 8;
    }
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
    if (port && !*port)
        return -1;                  /* "host:" with no port */
    if (port) {
        char *end;
        long p = strtol(port, &end, 10);
        if (*end || p < 1 || p > 65535)
            return -1;
        snprintf(t->port, sizeof t->port, "%ld", p);
    } else {
        strcpy(t->port, DEFAULT_PORT);
    }
    const char *path = slash ? slash : "/";
    if (strlen(path) > BH_MAX_PATH)
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
    struct timeval tv = { g_timeout, 0 };
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

typedef struct {
    const char *method;
    char *names[MAX_EXTRA];         /* from -H, lowercased and validated */
    const char *values[MAX_EXTRA];
    int nextra;
    int grease;
} options;

/* Split and check a -H 'name: value' argument against SPEC §5. */
static void add_header(options *o, const char *arg)
{
    const char *colon = strchr(arg, ':');
    if (o->nextra == MAX_EXTRA) {
        fprintf(stderr, "bcurl: at most %d -H options\n", MAX_EXTRA);
        exit(EX_USAGE);
    }
    size_t nl = colon ? (size_t)(colon - arg) : 0;
    char *name = malloc(nl + 1);
    if (!name)
        exit(EX_USAGE);
    for (size_t k = 0; k < nl; k++)
        name[k] = (char)tolower((unsigned char)arg[k]);
    name[nl] = '\0';
    const char *val = colon ? colon + 1 : "";
    while (*val == ' ')
        val++;
    if (!colon || !bh_valid_name(name) || !bh_valid_value(val)) {
        fprintf(stderr, "bcurl: bad header '%s' (want 'name: value', name in visible "
                        "ASCII without ':' or spaces, value under 32768 bytes)\n", arg);
        exit(EX_USAGE);
    }
    o->names[o->nextra] = name;
    o->values[o->nextra++] = val;
}

static int three_digits(const char *s)
{
    return s && s[0] >= '1' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9' &&
           s[2] >= '0' && s[2] <= '9' && s[3] == '\0';
}

/* The server reported a connection error on stream 0: show it and give up. */
static void report_connection_error(int fd, const bh_frame *f)
{
    int end = f->flags & BH_FLAG_END_STREAM;
    bh_frame d;
    fprintf(stderr, "bcurl: server reported a connection error");
    while (!end && bh_next_frame(fd, &d, g_trace) == 0 && d.type == BH_DATA && d.stream == 0 &&
           d.length <= 4096) {
        char msg[4097], safe[4 * 4096 + 8];
        if (bh_read_full(fd, msg, d.length) != 0)
            break;
        msg[d.length] = '\0';
        fprintf(stderr, ": %s", bh_escape(msg, safe, sizeof safe));
        end = d.flags & BH_FLAG_END_STREAM;
    }
    fputc('\n', stderr);
}

static int send_request(int fd, uint32_t sid, const target *t, const options *o)
{
    if (o->grease) {
        static const char junk[] = "unknown frame, please skip me";
        if (bh_send_frame(fd, BH_GREASE_TYPE, 0, 0, junk, sizeof junk - 1, g_trace) != 0)
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
    for (int i = 0; i < o->nextra; i++)
        bh_hb_add(&b, o->names[i], o->values[i]);
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
        fprintf(stderr, "bcurl: connection closed or timed out before a response arrived\n");
        return -1;
    }
    if (f.type == BH_HEADERS && f.stream == 0 && f.length <= BH_MAX_HEADER_BLOCK) {
        if (bh_read_payload(fd, &f, NULL, NULL) == 0)
            report_connection_error(fd, &f);
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
        if (three_digits(s) && bh_count(&h, ":status") == 1 && bh_count(&h, ":method") == 0 &&
            bh_count(&h, ":path") == 0)
            status = atoi(s);
        bh_headers_free(&h);
    }
    free(block);
    if (status < 0) {
        fprintf(stderr, "bcurl: response has a malformed header block or no valid :status\n");
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
                fprintf(stderr, "bcurl: truncated DATA frame or read timeout\n");
                return -1;
            }
            if (g_trace)
                bh_hexdump(g_trace, '<', chunk, n, done);
            if (fwrite(chunk, 1, n, stdout) != n) {
                fprintf(stderr, "bcurl: writing to stdout failed\n");
                return -1;
            }
            done += n;
        }
        end = f.flags & BH_FLAG_END_STREAM;
    }
    if (fflush(stdout) != 0) {
        fprintf(stderr, "bcurl: writing to stdout failed\n");
        return -1;
    }
    return status;
}

int main(int argc, char **argv)
{
    options o = { .method = "GET" };
    const char **urls = calloc((size_t)argc, sizeof *urls);
    int nurls = 0;
    if (!urls)
        return EX_USAGE;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-v") == 0)
            g_trace = stderr;
        else if (strcmp(a, "-I") == 0)
            o.method = "HEAD";
        else if (strcmp(a, "-X") == 0 && i + 1 < argc)
            o.method = argv[++i];
        else if (strcmp(a, "-H") == 0 && i + 1 < argc)
            add_header(&o, argv[++i]);
        else if (strcmp(a, "-t") == 0 && i + 1 < argc && (g_timeout = atoi(argv[i + 1])) > 0)
            i++;
        else if (strcmp(a, "--grease") == 0)
            o.grease = 1;
        else if (a[0] == '-')
            usage();
        else
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
    if ((uint32_t)nurls > BH_MAX_STREAM) {
        fprintf(stderr, "bcurl: too many requests for one connection\n");
        return EX_USAGE;
    }
    target *ts = calloc((size_t)nurls, sizeof *ts);
    if (!ts)
        return EX_USAGE;
    ts[0] = base;
    for (int i = 1; i < nurls; i++) {
        ts[i] = base;
        if (urls[i][0] == '/') {
            if (strlen(urls[i]) > BH_MAX_PATH) {
                fprintf(stderr, "bcurl: path longer than %u bytes\n", BH_MAX_PATH);
                return EX_USAGE;
            }
            strcpy(ts[i].path, urls[i]);
        } else if (parse_url(urls[i], &ts[i]) != 0 || strcmp(ts[i].host, base.host) != 0 ||
                   strcmp(ts[i].port, base.port) != 0) {
            fprintf(stderr, "bcurl: '%s' is not on %s:%s; bcurl never opens a second connection\n",
                    urls[i], base.host, base.port);
            return EX_USAGE;
        }
    }

    /* With fd 1 closed, socket() would hand out fd 1 and the body would be
     * written back into the connection. */
    if (fcntl(STDOUT_FILENO, F_GETFD) < 0) {
        fprintf(stderr, "bcurl: stdout is closed\n");
        return EX_PROTO;
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
