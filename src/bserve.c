/*
 * bserve - BHTTP/1 file server (Track 1).
 *
 *   usage: bserve [-v] [-t seconds] <root-dir> <port>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME     "bserve/1.0"
#define MAX_CHILDREN    128  /* concurrent connections; more are refused */
#define MAX_PER_CLIENT  32   /* connections from one address; more are refused */

static char  g_root[PATH_MAX];
static FILE *g_trace;               /* stderr when -v, else NULL */
static char  g_peer[64];
/* -t: seconds a connection may wait for the next request, seconds to receive
 * the rest of a request once it has started, and seconds the client has to
 * take each frame we send. */
static int   g_timeout = 30;

/* Log one line. Paths come from the peer, so escape control bytes. */
static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char msg[1200], safe[4800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[bserve %d] %s %s\n", (int)getpid(), g_peer,
            bh_escape(msg, safe, sizeof safe));
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

/* A short text/plain error response. A HEAD request gets the headers only. */
static int send_error(int fd, uint32_t sid, int status, const char *detail, int head_only)
{
    char safe[256], body[512];
    bh_escape(detail, safe, sizeof safe);          /* never echo raw peer bytes */
    int n = snprintf(body, sizeof body, "%d %s\n%s\n", status, reason(status), safe);
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

/* Cut :path at '?' or '#' and percent-decode it. 0 on success, else 400. */
static int decode_path(const char *path, char *dec, size_t decsz)
{
    size_t d = 0;
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
        if (d + 1 >= decsz)
            return 400;
        dec[d++] = (char)c;
    }
    dec[d] = '\0';
    return 0;
}

static int inside_root(const char *real)
{
    size_t rl = strlen(g_root);
    if (rl == 1)
        rl = 0;                         /* root "/" */
    return strncmp(real, g_root, rl) == 0 && (real[rl] == '/' || real[rl] == '\0');
}

/* For a path that does not resolve: does what exists of it already lead
 * outside the root? Follows a dangling symlink's target and otherwise the
 * longest existing prefix, so a 404 never confirms anything out there.
 * Iterative with heap buffers, so a path full of slashes cannot exhaust
 * the stack. A symlink loop, or a chain of more than 8 dangling links,
 * counts as unresolvable (404). */
static int leads_outside(const char *start)
{
    size_t cap = PATH_MAX + BH_MAX_PATH + 16;
    char *path = malloc(cap), *real = malloc(PATH_MAX), *target = malloc(PATH_MAX);
    int result = 0, hops = 0;
    if (!path || !real || !target || strlen(start) >= cap)
        goto out;
    strcpy(path, start);
    for (;;) {
        if (realpath(path, real)) {
            result = !inside_root(real);
            break;
        }
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
            if (++hops > 8)
                break;                  /* loop or long chain: 404 */
            ssize_t n = readlink(path, target, PATH_MAX - 1);
            if (n < 0)
                break;
            target[n] = '\0';
            if (target[0] == '/') {
                strcpy(path, target);
            } else {                    /* relative to the link's directory */
                char *slash = strrchr(path, '/');
                size_t keep = (size_t)(slash - path) + 1;
                if (keep + (size_t)n >= cap)
                    break;
                memcpy(path + keep, target, (size_t)n + 1);
            }
            continue;
        }
        /* Drop the last component (and any run of slashes before it). */
        char *slash = strrchr(path, '/');
        while (slash && slash > path && slash[-1] == '/')
            slash--;
        if (!slash)
            break;
        if (slash == path) {            /* only "/" exists: outside unless the root is "/" */
            result = strlen(g_root) > 1;
            break;
        }
        *slash = '\0';
    }
out:
    free(path);
    free(real);
    free(target);
    return result;
}

/*
 * Map a decoded path to a regular file under g_root.
 * Returns 0 and fills out (at least PATH_MAX bytes) on success, else 403/404.
 */
static int resolve_path(const char *dec, char *out)
{
    /* ".." anywhere is refused; other dot segments (".env", ".git") are hidden. */
    int hidden = 0;
    for (const char *seg = dec; seg; ) {
        const char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.')
            return 403;
        if (len > 0 && seg[0] == '.')
            hidden = 1;
        seg = next ? next + 1 : NULL;
    }
    if (hidden)
        return 404;

    /* Room for the root, the whole :path and "/index.html". */
    char full[PATH_MAX + BH_MAX_PATH + 16];
    size_t dl = strlen(dec);
    snprintf(full, sizeof full, "%s%s%s", g_root, dec,
             dl && dec[dl - 1] == '/' ? "index.html" : "");
    struct stat st;
    if (!(dl && dec[dl - 1] == '/') && stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t fl = strlen(full);
        if (fl + sizeof "/index.html" > sizeof full)
            return 404;
        memcpy(full + fl, "/index.html", sizeof "/index.html");
    }

    char real[PATH_MAX];
    if (!realpath(full, real))          /* missing or unresolvable, EACCES included */
        return leads_outside(full) ? 403 : 404;
    /* Symlinks must not lead outside the root. */
    if (!inside_root(real))
        return 403;
    size_t rl = strlen(g_root) == 1 ? 0 : strlen(g_root);
    /* Nor to a hidden file or directory inside it. */
    if (strstr(real + rl, "/."))
        return 404;
    /* Only regular files: opening a FIFO or device could block forever. */
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode))
        return 404;
    strcpy(out, real);
    return 0;
}

/* Handle one decoded request. Returns -1 if the connection should be dropped. */
static int handle_request(int fd, uint32_t sid, const uint8_t *block, size_t len)
{
    bh_headers h;
    if (bh_hb_decode(block, len, &h) != 0)
        return send_error(fd, sid, 400, "malformed header block", 0);

    /* Checks run in the order of SPEC §6: 400, then 405, then 403/404. */
    const char *method = bh_get(&h, ":method");
    const char *path   = bh_get(&h, ":path");
    int head = method && strcmp(method, "HEAD") == 0;
    char dec[BH_MAX_PATH + 1], file[PATH_MAX];
    int rc, status = 0;
    const char *why = path;

    if (!method || !path || path[0] != '/' || strlen(path) > BH_MAX_PATH) {
        status = 400;
        why = "request needs one :method and one :path of 1 to 1024 bytes starting with /";
    } else if (bh_get(&h, ":status")) {
        status = 400;
        why = ":status is not allowed in a request";
    } else if (!bh_valid_name_any_case(method)) {
        status = 400;
        why = ":method must be a token";
    } else if (decode_path(path, dec, sizeof dec) != 0) {
        status = 400;
    } else if (!head && strcmp(method, "GET") != 0) {
        status = 405;
        why = "only GET and HEAD are supported";
    } else {
        status = resolve_path(dec, file);
    }
    if (status) {
        rc = send_error(fd, sid, status, why, head);
        goto out;
    }

    /* realpath() already resolved every link. O_NOFOLLOW stops the last
     * component being swapped for a symlink since then (a swapped parent
     * directory is not caught; that needs write access to the root),
     * O_NONBLOCK stops a FIFO from blocking us, and fstat checks what we
     * actually opened. */
    int ffd = open(file, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    if (ffd < 0 || fstat(ffd, &st) != 0 || !S_ISREG(st.st_mode)) {
        int e = ffd < 0 ? errno : ENOENT;
        if (ffd >= 0)
            close(ffd);
        rc = send_error(fd, sid, e == EACCES ? 403 :
                        e == ENOENT || e == ENOTDIR || e == ELOOP ? 404 : 500, path, head);
        goto out;
    }

    long long remaining = st.st_size;
    rc = send_response_headers(fd, sid, 200, mime_type(file), remaining, head || remaining == 0);
    if (rc == 0)
        logf_("stream=%u %s %s -> 200 (%lld bytes)", sid, method, path, (long long)st.st_size);
    if (rc == 0 && !head) {
        uint8_t chunk[BH_DATA_CHUNK];
        while (remaining > 0) {
            ssize_t r = read(ffd, chunk, sizeof chunk);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                /* Read error, or the file shrank. A short body ended with
                 * END_STREAM would look complete, so drop the connection
                 * instead (SPEC §4). */
                logf_("stream=%u aborted: file read failed", sid);
                rc = -1;
                break;
            }
            if (r > remaining)
                r = (ssize_t)remaining;
            remaining -= r;
            rc = bh_send_frame(fd, BH_DATA, remaining == 0 ? BH_FLAG_END_STREAM : 0,
                               sid, chunk, (uint32_t)r, g_trace);
            if (rc != 0) {
                logf_("stream=%u aborted: %s", sid, errno == ETIMEDOUT
                      ? "client did not take a frame within the timeout" : "write failed");
                break;
            }
        }
    }
    close(ffd);
out:
    bh_headers_free(&h);
    return rc;
}

/* A connection error (SPEC §6): report it as a 400 on stream 0, then hang up.
 * Reading what the client already sent before closing keeps the kernel from
 * answering with an RST that could destroy the 400 in flight. */
static void connection_error(int fd, const char *why)
{
    send_error(fd, 0, 400, why, 0);
    shutdown(fd, SHUT_WR);
    /* Every read waits through bh_read_full, so the 2 s deadline holds even
     * if the client sends one more byte and then goes quiet. */
    bh_set_deadline(2);
    char junk[4096];
    while (bh_read_full(fd, junk, 1) == 0 && recv(fd, junk, sizeof junk, MSG_DONTWAIT) != 0)
        ;
}

static void serve_connection(int fd)
{
    uint32_t last_sid = 0;
    logf_("connected");
    for (;;) {
        bh_frame f;
        bh_set_deadline(g_timeout);             /* idle wait for the next request */
        int r = bh_next_frame(fd, &f, g_trace);
        bh_set_deadline(g_timeout);             /* the rest of this request */
        if (r == 1) {
            logf_("client closed connection");
            break;
        }
        if (r < 0) {
            logf_("connection dropped (%s)", errno == ETIMEDOUT || errno == EAGAIN
                  ? "timed out" : "read error or truncated frame");
            break;
        }
        if (f.type == BH_DATA) {
            bh_read_payload(fd, &f, NULL, g_trace);
            connection_error(fd, "DATA frame outside of a request");
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
            connection_error(fd, "stream id must be non-zero and increasing");
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
                bh_read_payload(fd, &d, NULL, g_trace);
                connection_error(fd, "expected DATA for the open request");
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
            r = send_error(fd, f.stream, 400, "header block over 65535 bytes", 0);
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

/* Children and the client address each one serves, for the per-client cap. */
static struct { pid_t pid; char addr[INET6_ADDRSTRLEN]; } g_child[MAX_CHILDREN];
static int g_nchild;

static void on_sigchld(int sig)
{
    (void)sig;
}

static void remember_child(pid_t pid, const char *addr)
{
    if (g_nchild >= MAX_CHILDREN)
        return;                         /* cannot happen: main() reaps below the cap */
    g_child[g_nchild].pid = pid;
    snprintf(g_child[g_nchild].addr, sizeof g_child[0].addr, "%s", addr);
    g_nchild++;
}

static void forget_child(pid_t pid)
{
    for (int i = 0; i < g_nchild; i++)
        if (g_child[i].pid == pid) {
            g_child[i] = g_child[--g_nchild];
            return;
        }
}

static int count_client(const char *addr)
{
    int n = 0;
    for (int i = 0; i < g_nchild; i++)
        n += strcmp(g_child[i].addr, addr) == 0;
    return n;
}

static void peer_addr(const struct sockaddr_storage *ss, char *out, size_t n)
{
    snprintf(out, n, "?");
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const void *)ss;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr))  /* ::ffff:1.2.3.4 is 1.2.3.4 */
            inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], out, (socklen_t)n);
        else
            inet_ntop(AF_INET6, &a->sin6_addr, out, (socklen_t)n);
    } else if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const void *)ss;
        inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)n);
    }
}

static void peer_name(const struct sockaddr_storage *ss)
{
    char host[INET6_ADDRSTRLEN];
    peer_addr(ss, host, sizeof host);
    int port = ss->ss_family == AF_INET6 ? ntohs(((const struct sockaddr_in6 *)(const void *)ss)->sin6_port)
             : ss->ss_family == AF_INET  ? ntohs(((const struct sockaddr_in *)(const void *)ss)->sin_port)
             : 0;
    snprintf(g_peer, sizeof g_peer, "%s:%d", host, port);
}

int main(int argc, char **argv)
{
    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (strcmp(argv[argi], "-v") == 0) {
            g_trace = stderr;
        } else if (strcmp(argv[argi], "-t") == 0 && argi + 1 < argc &&
                   (g_timeout = bh_parse_seconds(argv[argi + 1])) > 0) {
            argi++;
        } else {
            argi = argc;        /* force the usage message */
            break;
        }
    }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [-v] [-t seconds] <root-dir> <port>\n", argv[0]);
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
    /* SIGCHLD interrupts accept() so finished children are reaped promptly. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigaction(SIGCHLD, &sa, NULL);

    int lfd = listen_on(argv[argi + 1]);
    if (lfd < 0)
        return 1;
    fprintf(stderr, "bserve: serving %s on port %s (BHTTP/1)\n", g_root, argv[argi + 1]);

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
        int accept_errno = errno;       /* waitpid below would overwrite it */
        pid_t done;
        while ((done = waitpid(-1, NULL, WNOHANG)) > 0)   /* also after SIGCHLD's EINTR */
            forget_child(done);
        if (cfd < 0) {
            if (accept_errno != EINTR)
                fprintf(stderr, "bserve: accept: %s\n", strerror(accept_errno));
            if (accept_errno == EMFILE || accept_errno == ENFILE)
                usleep(100000);     /* out of descriptors: back off, don't spin */
            continue;
        }
        /* Children were reaped above without blocking, so one busy client
         * cannot stall accept for everyone. Now apply the caps. */
        char addr[INET6_ADDRSTRLEN];
        peer_addr(&ss, addr, sizeof addr);
        if (g_nchild >= MAX_CHILDREN || count_client(addr) >= MAX_PER_CLIENT) {
            fprintf(stderr, "[bserve] connection from %s refused: %s\n", addr,
                    g_nchild >= MAX_CHILDREN ? "server full" : "too many from this address");
            close(cfd);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(lfd);
            signal(SIGCHLD, SIG_DFL);
            peer_name(&ss);
            /* Each frame must be taken by the client within -t seconds; the
             * 1 s SO_SNDTIMEO lets bh_send_frame check that deadline. */
            int one = 1;
            struct timeval tv = { 1, 0 };
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            bh_set_frame_timeout(g_timeout);
            serve_connection(cfd);
            _exit(0);
        }
        if (pid < 0)
            perror("bserve: fork");
        else
            remember_child(pid, addr);
        close(cfd);
    }
}
