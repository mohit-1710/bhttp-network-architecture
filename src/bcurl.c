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
 * Exit status: 0 all responses below 400, 1 usage, 2 cannot connect,
 *              3 protocol, timeout or output error, 4 a 4xx, 5 a 5xx response.
 */
#include "cwire.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_PORT "9000"
#define USER_AGENT   "bcurl/1.0"
#define MAX_EXTRA    32

enum { EX_OK = 0, EX_USAGE = 1, EX_CONNECT = 2, EX_PROTO = 3, EX_4XX = 4, EX_5XX = 5 };

static FILE *g_trace;
static int   g_timeout = 30;        /* -t: connect timeout, and the time each response
                                       frame has to arrive in full */

static void usage(void)
{
    fprintf(stderr,
            "usage: bcurl [-v] [-I] [-X METHOD] [-H 'name: value']... [-t seconds] [--grease]\n"
            "             [bhttp://]host[:port][/path] [/more/paths ...]\n");
    exit(EX_USAGE);
}

typedef struct { char host[256]; char port[8]; char path[CW_MAX_PATH + 2]; } target;

/* Parse [bhttp://]host[:port][/path][?query], with [v6]:port allowed. */
static int parse_url(const char *url, target *t)
{
    /* A scheme is letters followed by "://" at the very start. */
    size_t sl = 0;
    while (isalpha((unsigned char)url[sl]))
        sl++;
    if (sl > 0 && strncmp(url + sl, "://", 3) == 0) {
        if (sl != 5 || strncasecmp(url, "bhttp", 5) != 0)
            return -1;              /* http://, https:// ... are not BHTTP */
        url += sl + 3;
    }
    size_t alen = strcspn(url, "/?#");
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
    /* A host name or IPv4 address, or (inside brackets) an IPv6 address. */
    const char *ok = auth[0] == '[' ? "0123456789abcdefABCDEF:.%" :
                     "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-";
    if (strspn(host, ok) != strlen(host))
        return -1;
    strcpy(t->host, host);
    if (port) {
        if (!*port || strlen(port) > 5 || strspn(port, "0123456789") != strlen(port) ||
            atoi(port) < 1 || atoi(port) > 65535)
            return -1;
        snprintf(t->port, sizeof t->port, "%d", atoi(port));
    } else {
        strcpy(t->port, DEFAULT_PORT);
    }
    /* "host?x" means "/?x". A #fragment never leaves the client. */
    const char *rest = url + alen;
    size_t rlen = strcspn(rest, "#");
    size_t plen = rlen + (*rest != '/');
    if (plen > CW_MAX_PATH)
        return -1;
    snprintf(t->path, sizeof t->path, "%s%.*s", *rest == '/' ? "" : "/", (int)rlen, rest);
    return cw_valid_value(t->path) ? 0 : -1;
}

/* Connect with a timeout: a non-blocking connect, then poll. */
static int connect_with_timeout(int fd, const struct sockaddr *sa, socklen_t len)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(fd, sa, len);
    if (rc != 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, g_timeout * 1000) == 1 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
            rc = 0;
    }
    fcntl(fd, F_SETFL, fl);
    return rc;
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
    /* Try each address in turn; close the failures, keep the first that connects. */
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen) == 0)
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
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   /* a server that never reads */
    return fd;
}

typedef struct {
    const char *method;
    char *names[MAX_EXTRA];         /* from -H, lowercased and validated */
    const char *values[MAX_EXTRA];
    int nextra;
    int grease;
    int show_headers;               /* -I: print response headers to stdout */
} options;

static const char *find_extra(const options *o, const char *name)
{
    for (int i = 0; i < o->nextra; i++)
        if (strcmp(o->names[i], name) == 0)
            return o->values[i];
    return NULL;
}

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
    while (*val == ' ' || *val == '\t')
        val++;
    char *v = strdup(val);              /* trailing spaces and tabs are trimmed too */
    if (!v)
        exit(EX_USAGE);
    for (size_t k = strlen(v); k > 0 && (v[k - 1] == ' ' || v[k - 1] == '\t'); k--)
        v[k - 1] = '\0';
    val = v;
    if (strcmp(name, "content-length") == 0) {
        fprintf(stderr, "bcurl: requests carry no body, so -H content-length is not allowed\n");
        exit(EX_USAGE);
    }
    if (find_extra(o, name) && (!strcmp(name, "host") || !strcmp(name, "user-agent") ||
                                !strcmp(name, "accept"))) {
        fprintf(stderr, "bcurl: -H %s given twice\n", name);
        exit(EX_USAGE);
    }
    if (!colon || !cw_valid_name(name) || !cw_valid_value(val)) {
        fprintf(stderr, "bcurl: bad header '%s' (want 'name: value', name made of HTTP "
                        "token characters, value under 32768 bytes without CR/LF)\n", arg);
        exit(EX_USAGE);
    }
    o->names[o->nextra] = name;
    o->values[o->nextra++] = val;
}

static int valid_method(const char *m)
{
    return strlen(m) <= 32 && cw_valid_name_any_case(m);
}

static int valid_status(const char *s)
{
    return s && s[0] >= '2' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9' &&
           s[2] >= '0' && s[2] <= '9' && s[3] == '\0';
}

static const char *closed_or_timed_out(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? "timed out"
                                                                         : "closed";
}

static int send_request(int fd, uint32_t sid, const target *t, const options *o)
{
    if (o->grease) {
        static const char junk[] = "unknown frame, please skip me";
        if (cw_send_frame(fd, CW_GREASE_TYPE, 0, 0, junk, sizeof junk - 1, g_trace) != 0)
            return -1;
    }
    char authority[300];
    if (strchr(t->host, ':'))
        snprintf(authority, sizeof authority, "[%s]:%s", t->host, t->port);
    else
        snprintf(authority, sizeof authority, "%s:%s", t->host, t->port);

    /* -H host/user-agent/accept replace the defaults rather than repeat them. */
    const char *host = find_extra(o, "host"), *ua = find_extra(o, "user-agent"),
               *accept = find_extra(o, "accept");
    cw_buf b = { 0 };
    cw_hb_add(&b, ":method", o->method);
    cw_hb_add(&b, ":path", t->path);
    cw_hb_add(&b, "host", host ? host : authority);
    cw_hb_add(&b, "user-agent", ua ? ua : USER_AGENT);
    cw_hb_add(&b, "accept", accept ? accept : "*/*");
    for (int i = 0; i < o->nextra; i++)
        if (strcmp(o->names[i], "host") && strcmp(o->names[i], "user-agent") &&
            strcmp(o->names[i], "accept"))
            cw_hb_add(&b, o->names[i], o->values[i]);
    int rc = -1;
    if (b.failed || b.len > CW_MAX_HEADER_BLOCK)
        fprintf(stderr, "bcurl: request headers exceed %u bytes\n", CW_MAX_HEADER_BLOCK);
    else
        rc = cw_send_frame(fd, CW_HEADERS, CW_END_STREAM, sid, b.data, (uint32_t)b.len,
                           g_trace);
    cw_buf_free(&b);
    return rc;
}

/* Read and decode a header block of f->length bytes (length already checked). */
static int read_block(int fd, const cw_frame *f, cw_headers *h)
{
    uint8_t *block = malloc(f->length ? f->length : 1);
    if (!block || cw_read_payload(fd, f, block, g_trace) != 0) {
        free(block);
        fprintf(stderr, "bcurl: connection %s inside a HEADERS frame\n", closed_or_timed_out());
        return -1;
    }
    if (g_trace)
        cw_trace_fields(g_trace, '<', block, f->length);
    int rc = cw_hb_decode(block, f->length, h);
    free(block);
    if (rc != 0)
        fprintf(stderr, "bcurl: malformed response header block\n");
    return rc;
}

/* A HEADERS frame on stream 0 is the server reporting a connection error
 * (SPEC §6). Print the reason from its body and give up. */
static void report_connection_error(int fd, const cw_frame *f)
{
    cw_headers h;
    if (read_block(fd, f, &h) == 0)
        cw_headers_free(&h);
    char msg[4097], safe[4 * 4096 + 8];
    size_t used = 0;
    int end = f->flags & CW_END_STREAM;
    cw_frame d;
    int cut = 0;
    while (!end && cw_next_frame(fd, &d, g_trace) == 0 && d.type == CW_DATA && d.stream == 0) {
        /* Keep what fits of the reason; read and drop the rest. */
        uint8_t *buf = malloc(d.length ? d.length : 1);
        if (!buf || cw_read_payload(fd, &d, buf, g_trace) != 0) {
            free(buf);
            break;
        }
        size_t take = d.length < sizeof msg - 1 - used ? d.length : sizeof msg - 1 - used;
        cut |= take < d.length;
        memcpy(msg + used, buf, take);
        used += take;
        free(buf);
        end = d.flags & CW_END_STREAM;
    }
    msg[used] = '\0';
    fprintf(stderr, "bcurl: server reported a connection error: %s%s\n",
            cw_escape(msg, safe, sizeof safe), cut || !end ? " ...(truncated)" : "");
}

/* Read one response on stream sid. Returns the status code, or -1. */
static int read_response(int fd, uint32_t sid, int head, int show_headers)
{
    cw_frame f;
    /* Each HEADERS or DATA frame must arrive in full within -t seconds.
     * Unknown frames and trickled bytes do not extend that. */
    cw_set_deadline(g_timeout);
    if (cw_next_frame(fd, &f, g_trace) != 0) {
        fprintf(stderr, "bcurl: connection %s before a response arrived\n", closed_or_timed_out());
        return -1;
    }
    if (f.type == CW_HEADERS && f.length > CW_MAX_HEADER_BLOCK) {
        fprintf(stderr, "bcurl: response header block over %u bytes\n", CW_MAX_HEADER_BLOCK);
        return -1;
    }
    if (f.type == CW_HEADERS && f.stream == 0) {
        report_connection_error(fd, &f);
        return -1;
    }
    if (f.type != CW_HEADERS || f.stream != sid) {
        fprintf(stderr, "bcurl: expected HEADERS on stream %u, got %s on stream %u\n", sid,
                f.type == CW_DATA ? "DATA" : "HEADERS", f.stream);
        return -1;
    }

    cw_headers h;
    if (read_block(fd, &f, &h) != 0)
        return -1;
    const char *s = cw_get(&h, ":status");
    const char *cl = cw_get(&h, "content-length");
    int status = -1;
    long long want = -1;
    if (valid_status(s) && !cw_get(&h, ":method") && !cw_get(&h, ":path"))
        status = atoi(s);
    if (show_headers && status > 0) {
        char vb[4 * 1024 + 8];
        for (int i = 0; i < h.n; i++)
            printf("%s: %s\n", h.fields[i].name, cw_escape(h.fields[i].value, vb, sizeof vb));
    }
    if (cl && *cl && strspn(cl, "0123456789") == strlen(cl) && strlen(cl) < 19)
        want = atoll(cl);
    else if (cl)
        status = -1;
    cw_headers_free(&h);
    if (status < 0) {
        fprintf(stderr, "bcurl: response has no valid :status (200-599) or a bad content-length\n");
        return -1;
    }

    long long got = 0;
    int end = f.flags & CW_END_STREAM;
    /* A 400 may carry a body even for HEAD: the server may not have been
     * able to read the method (SPEC §4). */
    if (head && !end && status != 400) {
        fprintf(stderr, "bcurl: response to HEAD does not end with its HEADERS frame\n");
        return -1;
    }
    int fresh = 1;                      /* restart the deadline for this frame? */
    while (!end) {
        if (fresh)
            cw_set_deadline(g_timeout);
        if (cw_next_frame(fd, &f, g_trace) != 0) {
            fprintf(stderr, "bcurl: connection %s mid-response (body incomplete)\n",
                    closed_or_timed_out());
            return -1;
        }
        if (f.type == CW_HEADERS && f.stream == 0 && f.length <= CW_MAX_HEADER_BLOCK) {
            report_connection_error(fd, &f);
            return -1;
        }
        if (f.type != CW_DATA || f.stream != sid) {
            fprintf(stderr, "bcurl: expected DATA on stream %u\n", sid);
            return -1;
        }
        /* Stop before writing anything past content-length. */
        if (want >= 0 && got + f.length > want) {
            fprintf(stderr, "bcurl: body is longer than content-length %lld\n", want);
            return -1;
        }
        /* Stream the payload to stdout without buffering the whole body. */
        uint8_t chunk[CW_CHUNK];
        size_t done = 0;
        while (done < f.length) {
            size_t n = f.length - done;
            if (n > sizeof chunk)
                n = sizeof chunk;
            if (cw_read_full(fd, chunk, n) != 0) {
                fprintf(stderr, "bcurl: connection %s inside a DATA frame\n",
                        closed_or_timed_out());
                return -1;
            }
            if (g_trace)
                cw_hexdump(g_trace, '<', chunk, n, done);
            if (fwrite(chunk, 1, n, stdout) != n) {
                fprintf(stderr, "bcurl: writing to stdout failed\n");
                return -1;
            }
            done += n;
        }
        got += f.length;
        end = f.flags & CW_END_STREAM;
        /* Only a frame that carried bytes earns a new deadline, so a server
         * cannot hold bcurl open with empty DATA frames. */
        fresh = f.length > 0;
    }
    if (fflush(stdout) != 0) {
        fprintf(stderr, "bcurl: writing to stdout failed\n");
        return -1;
    }
    /* A HEAD response's content-length describes the body it did not send;
     * only a 400 that actually carried DATA is checked against it. */
    if ((!head || got > 0) && want >= 0 && got != want) {
        fprintf(stderr, "bcurl: body is %lld bytes but content-length said %lld\n", got, want);
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
        else if (strcmp(a, "-I") == 0) {
            o.method = "HEAD";
            o.show_headers = 1;
        }
        else if (strcmp(a, "-X") == 0 && i + 1 < argc)
            o.method = argv[++i];
        else if (strcmp(a, "-H") == 0 && i + 1 < argc)
            add_header(&o, argv[++i]);
        else if (strcmp(a, "-t") == 0 && i + 1 < argc && cw_parse_seconds(argv[i + 1]) > 0)
            g_timeout = cw_parse_seconds(argv[++i]);
        else if (strcmp(a, "--grease") == 0)
            o.grease = 1;
        else if (a[0] == '-')
            usage();
        else
            urls[nurls++] = a;
    }
    if (nurls == 0)
        usage();
    if (!valid_method(o.method)) {
        fprintf(stderr, "bcurl: bad method (an HTTP token of at most 32 characters)\n");
        return EX_USAGE;
    }

    target base;
    if (parse_url(urls[0], &base) != 0) {
        fprintf(stderr, "bcurl: bad URL '%s'\n", urls[0]);
        return EX_USAGE;
    }
    /* Every later argument must address the same server: one connection only. */
    if ((uint32_t)nurls > CW_MAX_STREAM) {
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
            size_t pl = strcspn(urls[i], "#");     /* a #fragment is not sent */
            if (pl > CW_MAX_PATH) {
                fprintf(stderr, "bcurl: bad path '%s'\n", urls[i]);
                return EX_USAGE;
            }
            memcpy(ts[i].path, urls[i], pl);
            ts[i].path[pl] = '\0';
            if (!cw_valid_value(ts[i].path)) {
                fprintf(stderr, "bcurl: bad path '%s'\n", urls[i]);
                return EX_USAGE;
            }
        } else if (parse_url(urls[i], &ts[i]) != 0 || strcasecmp(ts[i].host, base.host) != 0 ||
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

    int rc = EX_OK, head = strcmp(o.method, "HEAD") == 0;
    for (int i = 0; i < nurls; i++) {
        uint32_t sid = (uint32_t)i + 1;
        if (send_request(fd, sid, &ts[i], &o) != 0) {
            fprintf(stderr, "bcurl: failed to send request %u\n", sid);
            rc = EX_PROTO;
            break;
        }
        int status = read_response(fd, sid, head, o.show_headers);
        if (status < 0) {
            if (i + 1 < nurls)
                fprintf(stderr, "bcurl: %d later request(s) not sent\n", nurls - i - 1);
            rc = EX_PROTO;
            break;
        }
        if (g_trace) {
            char safe[4 * CW_MAX_PATH + 8];
            fprintf(g_trace, "* stream %u: %s %s -> %d\n", sid, o.method,
                    cw_escape(ts[i].path, safe, sizeof safe), status);
        }
        if (status >= 500)
            rc = EX_5XX;
        else if (status >= 400 && rc != EX_5XX)
            rc = EX_4XX;
    }
    close(fd);
    if (g_trace)
        fprintf(g_trace, "* connection closed, exit %d\n", rc);
    for (int i = 0; i < o.nextra; i++) {
        free(o.names[i]);
        free((char *)o.values[i]);
    }
    free(ts);
    free(urls);
    return rc;
}
