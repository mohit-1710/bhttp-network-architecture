/*
 * bserve - BHTTP/1 file server (Track 1).
 *
 *   usage: bserve [-v] <root-dir> <port>
 *
 * Accepts TCP connections, reads binary request frames, maps :path to a file
 * under <root-dir>, and answers with a HEADERS frame plus DATA frames. The
 * connection stays open for further requests until the client closes it.
 */
#include "bproto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME  "bserve/1.0"
#define IDLE_TIMEOUT 30   /* seconds a keep-alive connection may sit idle */

static char  g_root[PATH_MAX];
static FILE *g_trace;               /* stderr when -v, else NULL */
static char  g_peer[64];

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bserve %d] %s ", (int)getpid(), g_peer);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

static const char *mime_type(const char *path)
{
    static const struct { const char *ext, *type; } map[] = {
        { ".html", "text/html; charset=utf-8" }, { ".htm", "text/html; charset=utf-8" },
        { ".txt", "text/plain; charset=utf-8" },  { ".css", "text/css" },
        { ".js", "text/javascript" },             { ".json", "application/json" },
        { ".png", "image/png" },                  { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },                { ".gif", "image/gif" },
        { ".svg", "image/svg+xml" },              { ".ico", "image/x-icon" },
        { ".pdf", "application/pdf" },            { ".md", "text/markdown; charset=utf-8" },
    };
    const char *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcasecmp(dot, map[i].ext) == 0)
                return map[i].type;
    return "application/octet-stream";
}

static void http_date(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* Send the HEADERS frame of a response. */
static int send_response_headers(int fd, uint32_t sid, int status, const char *ctype,
                                 long long length, int end_stream)
{
    char st[8], len[32], date[64];
    snprintf(st, sizeof st, "%d", status);
    snprintf(len, sizeof len, "%lld", length);
    http_date(date, sizeof date);

    bh_buf b = { 0 };
    bh_hb_add(&b, ":status", st);
    bh_hb_add(&b, "server", SERVER_NAME);
    bh_hb_add(&b, "date", date);
    bh_hb_add(&b, "content-type", ctype);
    bh_hb_add(&b, "content-length", len);
    int rc = b.err ? -1
                   : bh_send_frame(fd, BH_HEADERS, end_stream ? BH_FLAG_END_STREAM : 0,
                                   sid, b.buf, (uint32_t)b.len, g_trace);
    bh_buf_free(&b);
    return rc;
}

/* A short text/plain error response. */
static int send_error(int fd, uint32_t sid, int status, const char *detail, int head_only)
{
    char body[512];
    int n = snprintf(body, sizeof body, "%d %s\n%s\n", status, reason(status), detail);
    if (n < 0 || (size_t)n >= sizeof body)
        n = (int)strlen(body);
    logf_("stream=%u -> %d (%s)", sid, status, detail);
    if (send_response_headers(fd, sid, status, "text/plain; charset=utf-8", n, head_only) != 0)
        return -1;
    if (head_only)
        return 0;
    return bh_send_frame(fd, BH_DATA, BH_FLAG_END_STREAM, sid, body, (uint32_t)n, g_trace);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Map a request path to a file under g_root.
 * Returns 0 and fills out on success, otherwise an HTTP status code.
 */
static int map_path(const char *path, char *out, size_t outsz)
{
    char dec[PATH_MAX];
    size_t d = 0;
    /* Percent-decode up to the query/fragment. */
    for (const char *s = path; *s && *s != '?' && *s != '#'; s++) {
        int c = (unsigned char)*s;
        if (c == '%') {
            int hi = hexval(s[1]), lo = hi < 0 ? -1 : hexval(s[2]);
            if (hi < 0 || lo < 0)
                return 400;
            c = hi << 4 | lo;
            s += 2;
            if (c == 0)
                return 400;
        }
        if (d + 1 >= sizeof dec)
            return 400;
        dec[d++] = (char)c;
    }
    dec[d] = '\0';

    /* Reject any ".." segment before touching the filesystem. */
    for (char *seg = dec; seg; ) {
        char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return 403;
        seg = next ? next + 1 : NULL;
    }

    char full[PATH_MAX];
    int n = snprintf(full, sizeof full, "%s%s%s", g_root, dec,
                     dec[d - 1] == '/' ? "index.html" : "");
    if (n < 0 || (size_t)n >= sizeof full)
        return 400;

    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (strlen(full) + sizeof "/index.html" > sizeof full)
            return 400;
        strcat(full, "/index.html");
    }

    char real[PATH_MAX];
    if (!realpath(full, real))
        return (errno == EACCES) ? 403 : 404;
    /* Symlinks must not lead outside the root. */
    size_t rl = strlen(g_root);
    if (strncmp(real, g_root, rl) != 0 || (real[rl] != '/' && real[rl] != '\0'))
        return 403;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode))
        return 404;
    if (strlen(real) >= outsz)
        return 400;
    strcpy(out, real);
    return 0;
}

/* Handle one decoded request. Returns -1 if the connection should be dropped. */
static int handle_request(int fd, uint32_t sid, const uint8_t *block, size_t len)
{
    bh_headers h;
    if (bh_hb_decode(block, len, &h) != 0)
        return send_error(fd, sid, 400, "malformed header block", 0);

    const char *method = bh_get(&h, ":method");
    const char *path   = bh_get(&h, ":path");
    int rc;
    if (!method || !path || bh_count(&h, ":method") != 1 || bh_count(&h, ":path") != 1 ||
        bh_get(&h, ":status") || path[0] != '/') {
        rc = send_error(fd, sid, 400, "request needs exactly one :method and one absolute :path", 0);
        goto out;
    }

    int head = strcmp(method, "HEAD") == 0;
    if (!head && strcmp(method, "GET") != 0) {
        rc = send_error(fd, sid, 405, "only GET and HEAD are supported", 0);
        goto out;
    }

    char file[PATH_MAX];
    int status = map_path(path, file, sizeof file);
    if (status) {
        rc = send_error(fd, sid, status, path, head);
        goto out;
    }

    int ffd = open(file, O_RDONLY);
    struct stat st;
    if (ffd < 0 || fstat(ffd, &st) != 0) {
        int e = errno;
        if (ffd >= 0)
            close(ffd);
        rc = send_error(fd, sid, e == EACCES ? 403 : 404, path, head);
        goto out;
    }

    long long remaining = st.st_size;
    rc = send_response_headers(fd, sid, 200, mime_type(file), remaining, head || remaining == 0);
    logf_("stream=%u %s %s -> 200 (%lld bytes)", sid, method, path, (long long)st.st_size);
    if (rc == 0 && !head) {
        uint8_t chunk[BH_DATA_CHUNK];
        while (remaining > 0) {
            ssize_t r = read(ffd, chunk, sizeof chunk);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                /* File shrank under us: end the stream cleanly anyway. */
                rc = bh_send_frame(fd, BH_DATA, BH_FLAG_END_STREAM, sid, NULL, 0, g_trace);
                break;
            }
            if (r > remaining)
                r = (ssize_t)remaining;
            remaining -= r;
            rc = bh_send_frame(fd, BH_DATA, remaining == 0 ? BH_FLAG_END_STREAM : 0,
                               sid, chunk, (uint32_t)r, g_trace);
            if (rc != 0)
                break;
        }
    }
    close(ffd);
out:
    bh_headers_free(&h);
    return rc;
}

/* A framing-level error: answer 400 on the offending stream, then hang up. */
static void connection_error(int fd, uint32_t sid, const char *why)
{
    send_error(fd, sid, 400, why, 0);
    shutdown(fd, SHUT_WR);
}

static void serve_connection(int fd)
{
    uint32_t last_sid = 0;
    logf_("connected");
    for (;;) {
        bh_frame f;
        int r = bh_next_frame(fd, &f, g_trace);
        if (r == 1) {
            logf_("client closed connection");
            break;
        }
        if (r < 0) {
            logf_("connection dropped (read error, timeout or truncated frame)");
            break;
        }
        if (f.type == BH_DATA) {
            bh_read_payload(fd, &f, NULL, g_trace);
            connection_error(fd, f.stream, "DATA frame outside of a request");
            break;
        }
        /* HEADERS: a new request. */
        uint8_t *block = NULL;
        int oversized = f.length > BH_MAX_HEADER_BLOCK;
        if (!oversized && !(block = malloc(f.length ? f.length : 1)))
            break;
        if (bh_read_payload(fd, &f, block, g_trace) != 0) {
            free(block);
            logf_("truncated HEADERS frame");
            break;
        }
        /* Checked after the payload is consumed, so closing does not RST
         * away the 400 we are about to send. */
        if (f.stream == 0 || f.stream <= last_sid) {
            free(block);
            connection_error(fd, f.stream, "stream id must be non-zero and increasing");
            break;
        }
        last_sid = f.stream;
        if (block && g_trace)
            bh_trace_fields(g_trace, '<', block, f.length);

        /* Requests may carry a body (not used by GET/HEAD in v1): drain it. */
        int end = f.flags & BH_FLAG_END_STREAM, bad = 0;
        while (!end) {
            bh_frame d;
            if (bh_next_frame(fd, &d, g_trace) != 0) {
                bad = -1;
                break;
            }
            if (d.type != BH_DATA || d.stream != f.stream) {
                if (d.length <= BH_MAX_HEADER_BLOCK)
                    bh_read_payload(fd, &d, NULL, g_trace);
                connection_error(fd, f.stream, "expected DATA for the open request");
                bad = -1;
                break;
            }
            if (bh_read_payload(fd, &d, NULL, g_trace) != 0) {
                bad = -1;
                break;
            }
            end = d.flags & BH_FLAG_END_STREAM;
        }
        if (bad) {
            free(block);
            break;
        }

        if (oversized)
            r = send_error(fd, f.stream, 400, "header block too large", 0);
        else
            r = handle_request(fd, f.stream, block, f.length);
        free(block);
        if (r != 0)
            break;
    }
    close(fd);
}

static int listen_on(const char *port)
{
    char *end;
    long p = strtol(port, &end, 10);
    if (*end || p < 1 || p > 65535) {
        fprintf(stderr, "bserve: bad port '%s'\n", port);
        return -1;
    }
    int one = 1, zero = 0;
    /* Prefer a dual-stack IPv6 socket so both ::1 and 127.0.0.1 work. */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in6 a6 = { 0 };
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((uint16_t)p);
        a6.sin6_addr = in6addr_any;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(fd, 64) == 0)
            return fd;
        close(fd);
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("bserve: socket");
        return -1;
    }
    struct sockaddr_in a4 = { 0 };
    a4.sin_family = AF_INET;
    a4.sin_port = htons((uint16_t)p);
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&a4, sizeof a4) != 0 || listen(fd, 64) != 0) {
        perror("bserve: bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static void peer_name(const struct sockaddr_storage *ss)
{
    char host[INET6_ADDRSTRLEN] = "?";
    int port = 0;
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const void *)ss;
        inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
        port = ntohs(a->sin6_port);
    } else if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const void *)ss;
        inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
        port = ntohs(a->sin_port);
    }
    snprintf(g_peer, sizeof g_peer, "%s:%d", host, port);
}

int main(int argc, char **argv)
{
    int argi = 1;
    if (argi < argc && strcmp(argv[argi], "-v") == 0) {
        g_trace = stderr;
        argi++;
    }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [-v] <root-dir> <port>\n", argv[0]);
        return 1;
    }
    if (!realpath(argv[argi], g_root)) {
        fprintf(stderr, "bserve: root '%s': %s\n", argv[argi], strerror(errno));
        return 1;
    }
    struct stat st;
    if (stat(g_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: root '%s' is not a directory\n", g_root);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);   /* a vanished client must not kill us */
    signal(SIGCHLD, SIG_IGN);   /* children are reaped automatically */

    int lfd = listen_on(argv[argi + 1]);
    if (lfd < 0)
        return 1;
    fprintf(stderr, "bserve: serving %s on port %s (BHTTP/1)\n", g_root, argv[argi + 1]);

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
        if (cfd < 0) {
            if (errno != EINTR)
                perror("bserve: accept");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            peer_name(&ss);
            int one = 1;
            struct timeval tv = { IDLE_TIMEOUT, 0 };
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            serve_connection(cfd);
            _exit(0);
        }
        if (pid < 0)
            perror("bserve: fork");
        close(cfd);
    }
}
