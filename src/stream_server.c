/*
 * PKG Manager - Virtual HTTP Range Streaming Server
 *
 * Dedicated raw-socket HTTP server (port 8845) delivering deterministic
 * byte-range chunks directly to the PS5 installer subsystem.
 */

#include "stream_server.h"
#include "multipart.h"
#include "installer.h"
#include "stream_debug_log.h"
#include "smb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>

#define STREAM_SEND_CHUNK (2048 * 1024)
#define STREAM_REQ_MAX (16 * 1024)
#define STREAM_IO_TIMEOUT_SEC 30

typedef struct {
    int conn;
    int conn_id;
    char peer[64];
} stream_conn_arg_t;

/* Max simultaneously tracked connection fds for shutdown-all on stop. */
#define STREAM_MAX_TRACKED_FDS 32

typedef struct {
    int listen_fd;
    volatile int running;
    pthread_t thread;
    int thread_created;
    pthread_mutex_t mutex;
    virtual_stream_t *vs; /* heap (~138KB worth of parts), never thread stack */
    int vs_refs;          /* in-flight request handlers; stop waits for 0 */
    int stopping;         /* stop in progress: releases signal vs_cond */
    pthread_cond_t vs_cond;
    uint64_t total_size;
    int live_fds[STREAM_MAX_TRACKED_FDS]; /* -1 = free slot */
    char session_name[128]; /* pinned *.pkg basename; "" = legacy any-*.pkg */
} stream_session_t;

/* Verbose connection logging is gated behind PKG_DEBUG_STREAM (or
 * stream_server_set_debug(1)) to prevent flooding install logs with thousands
 * of per-chunk connection events. */
static volatile int g_stream_debug_override = -1; /* -1 = auto (env), 0 = off, 1 = on */

void stream_server_set_debug(int enable) {
    g_stream_debug_override = enable ? 1 : 0;
}

static int stream_debug_enabled(void) {
    if (g_stream_debug_override >= 0) {
        return g_stream_debug_override;
    }
    const char *dbg = getenv("PKG_DEBUG_STREAM");
    return (dbg && dbg[0] != '\0' && strcmp(dbg, "0") != 0);
}

#define stream_debug_log(...) do { \
    if (stream_debug_enabled()) { \
        install_log(__VA_ARGS__); \
    } \
} while (0)

static volatile int g_stream_conn_seq = 0;
static volatile int g_stream_active_workers = 0;

static void stream_format_peer(const struct sockaddr_in *cli, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }
    if (!cli) {
        snprintf(out, out_sz, "unknown");
        return;
    }
    char ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &cli->sin_addr, ip, sizeof(ip)) == NULL) {
        snprintf(out, out_sz, "unknown");
        return;
    }
    snprintf(out, out_sz, "%s:%u", ip, (unsigned int)ntohs(cli->sin_port));
}

static uint64_t stream_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/* Extracts a single header value (case-insensitive name) into out.
 * Returns 1 when found, 0 otherwise. Logging-only. */
static int stream_header_value(const char *req, const char *name, char *out, size_t out_sz) {
    size_t nlen;
    const char *p;
    if (!req || !name || !out || out_sz == 0) {
        return 0;
    }
    nlen = strlen(name);
    p = req;
    while (*p) {
        const char *line = p;
        const char *eol = strstr(line, "\r\n");
        if (!eol) {
            eol = strchr(line, '\n');
            if (!eol) {
                eol = line + strlen(line);
            }
        }
        /* Skip request line (no colon before SP) for robustness: match "Name:" prefix. */
        if ((size_t)(eol - line) > nlen + 1) {
            size_t i;
            int match = 1;
            for (i = 0; i < nlen; i++) {
                char a = line[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match && line[nlen] == ':') {
                const char *v = line + nlen + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t vlen = (size_t)(eol - v);
                if (vlen >= out_sz) vlen = out_sz - 1;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                /* Strip trailing whitespace/CR. */
                while (vlen > 0 && (out[vlen - 1] == ' ' || out[vlen - 1] == '\t' ||
                                    out[vlen - 1] == '\r' || out[vlen - 1] == '\n')) {
                    out[--vlen] = '\0';
                }
                return 1;
            }
        }
        if (*eol == '\0') break;
        p = (*eol == '\r' && *(eol + 1) == '\n') ? eol + 2 : eol + 1;
        /* Empty line = end of headers. */
        if (p[0] == '\r' || p[0] == '\n' || p[0] == '\0') break;
    }
    return 0;
}

static stream_session_t g_ss = {
    .listen_fd = -1,
    .running = 0,
    .thread_created = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .vs = NULL,
    .vs_refs = 0,
    .stopping = 0,
    .vs_cond = PTHREAD_COND_INITIALIZER,
    .total_size = 0,
    .live_fds = { [0 ... STREAM_MAX_TRACKED_FDS - 1] = -1 },
    .session_name = {0}
};

/* Register/unregister a connection fd so session_stop() can shut them
 * all down (unblocking recv/send/keep-alive-idle workers). */
static void stream_track_add(int fd) {
    pthread_mutex_lock(&g_ss.mutex);
    for (int i = 0; i < STREAM_MAX_TRACKED_FDS; i++) {
        if (g_ss.live_fds[i] < 0) {
            g_ss.live_fds[i] = fd;
            break;
        }
    }
    pthread_mutex_unlock(&g_ss.mutex);
}

static void stream_track_remove(int fd) {
    pthread_mutex_lock(&g_ss.mutex);
    for (int i = 0; i < STREAM_MAX_TRACKED_FDS; i++) {
        if (g_ss.live_fds[i] == fd) {
            g_ss.live_fds[i] = -1;
        }
    }
    pthread_mutex_unlock(&g_ss.mutex);
}

/* Pin the virtual stream for one request body. Returns NULL when the
 * session is stopping/stopped; every non-NULL return needs exactly one
 * stream_vs_release(). Keeps virtual_stream_close/free in stop() from
 * racing in-flight preads. */
static virtual_stream_t *stream_vs_acquire(void) {
    pthread_mutex_lock(&g_ss.mutex);
    virtual_stream_t *vp = NULL;
    if (g_ss.running && g_ss.vs != NULL) {
        vp = g_ss.vs;
        g_ss.vs_refs++;
    }
    pthread_mutex_unlock(&g_ss.mutex);
    return vp;
}

static void stream_vs_release(void) {
    pthread_mutex_lock(&g_ss.mutex);
    if (g_ss.vs_refs > 0) {
        g_ss.vs_refs--;
    }
    if (g_ss.stopping && g_ss.vs_refs == 0) {
        pthread_cond_signal(&g_ss.vs_cond);
    }
    pthread_mutex_unlock(&g_ss.mutex);
}

void stream_server_set_session_name(const char *session_name) {
    pthread_mutex_lock(&g_ss.mutex);
    if (!session_name || session_name[0] == '\0') {
        g_ss.session_name[0] = '\0';
    } else {
        /* Keep basename only: callers may pass a full URI. */
        const char *slash = strrchr(session_name, '/');
        const char *base = slash ? slash + 1 : session_name;
        strncpy(g_ss.session_name, base, sizeof(g_ss.session_name) - 1);
        g_ss.session_name[sizeof(g_ss.session_name) - 1] = '\0';
    }
    pthread_mutex_unlock(&g_ss.mutex);
}

int stream_server_is_running(void) {
    return g_ss.running;
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Returns 1 with out_start/out_end set for a satisfiable range,
 * 0 when no Range header is present, -1 when unsatisfiable. */
static int find_range(const char *req, uint64_t total, uint64_t *out_start, uint64_t *out_end) {
    /* Case-insensitive "Range:" header name (RFC 9110 field names are
     * case-insensitive); MHD lookup is case-insensitive, keep parity. */
    const char *rl = NULL;
    const char *scan = req;
    while ((scan = strchr(scan, '\n')) != NULL) {
        scan++;
        while (*scan == ' ' || *scan == '\t') scan++;
        if ((scan[0] == 'R' || scan[0] == 'r') &&
            (scan[1] == 'A' || scan[1] == 'a') &&
            (scan[2] == 'N' || scan[2] == 'n') &&
            (scan[3] == 'G' || scan[3] == 'g') &&
            (scan[4] == 'E' || scan[4] == 'e') &&
            scan[5] == ':') {
            rl = scan;
            break;
        }
    }
    if (!rl) {
        /* Also accept request-line-adjacent "Range:" without preceding \n
         * (tests / minimal clients). */
        const char *r1 = strstr(req, "Range:");
        const char *r2 = strstr(req, "range:");
        const char *r3 = strstr(req, "RANGE:");
        rl = r1;
        if (r2 && (!rl || r2 < rl)) rl = r2;
        if (r3 && (!rl || r3 < rl)) rl = r3;
    }
    if (!rl) {
        return 0;
    }
    const char *le = strstr(rl, "\r\n");
    const char *b = strstr(rl, "bytes=");
    if (!b || (le && b > le)) {
        return 0;
    }
    b += 6;
    const char *dash = strchr(b, '-');
    if (!dash || (le && dash > le)) {
        return 0;
    }
    uint64_t start = 0;
    uint64_t end = (total > 0) ? (total - 1) : 0;
    if (dash == b) {
        /* Suffix range: bytes=-N (last N bytes) */
        uint64_t suffix = strtoull(dash + 1, NULL, 10);
        if (total == 0) {
            return -1;
        }
        if (suffix > total) {
            suffix = total;
        }
        if (suffix == 0) {
            return -1;
        }
        start = total - suffix;
        end = total - 1;
    } else {
        start = strtoull(b, NULL, 10);
        if (*(dash + 1) != '\0' && (le == NULL || (dash + 1) < le)) {
            /* Only treat trailing digits on the same header line as end */
            const char *p = dash + 1;
            int has_digit = 0;
            while ((!le || p < le) && *p >= '0' && *p <= '9') {
                has_digit = 1;
                p++;
            }
            if (has_digit) {
                end = strtoull(dash + 1, NULL, 10);
            } else {
                end = (total > 0) ? (total - 1) : 0;
            }
        } else {
            end = (total > 0) ? (total - 1) : 0;
        }
    }
    if (total == 0 || start > end || start >= total) {
        return -1;
    }
    if (end >= total) {
        end = total - 1;
    }
    *out_start = start;
    *out_end = end;
    return 1;
}

typedef enum { SERVE_CLOSE = 0, SERVE_KEEP = 1 } serve_verdict_t;

/* Upper bound on sequential requests per keep-alive connection. */
#define STREAM_MAX_REQ_PER_CONN 100

static serve_verdict_t serve_one_request(int conn, int conn_id, const char *peer,
                                         uint64_t t_start_ms, int req_no,
                                         char *req, size_t rlen,
                                         smb_file_session_t *conn_smb);

static void serve_connection(int conn, int conn_id, const char *peer_str) {
    const char *peer = (peer_str && peer_str[0] != '\0') ? peer_str : "unknown";
    uint64_t t_start_ms = stream_now_ms();
    int workers_now = __sync_add_and_fetch(&g_stream_active_workers, 1);
    char peer_local[64];
    if (!peer_str || peer_str[0] == '\0') {
        struct sockaddr_in pn;
        socklen_t pnlen = sizeof(pn);
        memset(&pn, 0, sizeof(pn));
        if (getpeername(conn, (struct sockaddr *)&pn, &pnlen) == 0) {
            stream_format_peer(&pn, peer_local, sizeof(peer_local));
            peer = peer_local;
        }
    }

    struct timeval tv;
    tv.tv_sec = STREAM_IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* For SMB streams: open a dedicated SMB session for this connection so
     * concurrent range requests from the PS5 read from the NAS in parallel
     * over independent TCP connections without lock contention or seek jitter. */
    smb_file_session_t *conn_smb = NULL;
    virtual_stream_t *vp_init = stream_vs_acquire();
    if (vp_init != NULL) {
        const char *smb_url = virtual_stream_get_smb_url(vp_init);
        if (smb_url != NULL) {
            conn_smb = smb_file_session_open(smb_url);
            if (conn_smb) {
                install_log("[STREAM] conn #%d peer=%s opened private SMB session for parallel read",
                            conn_id, peer);
            } else {
                install_log("[STREAM] conn #%d peer=%s WARNING: failed to open private SMB session, using fallback",
                            conn_id, peer);
            }
        }
        stream_vs_release();
    }

    stream_debug_log("[STREAM] conn #%d start fd=%d peer=%s workers=%d",
                     conn_id, conn, peer, workers_now);

    /* Keep-alive loop: serve sequential requests on one socket. The 30s
     * receive timeout above bounds idle waits between requests, and the
     * per-connection request cap bounds a stuck client. */
    char carry[STREAM_REQ_MAX + 1];
    size_t carry_len = 0;
    int req_no = 0;
    int reqs_served = 0;
    while (req_no < STREAM_MAX_REQ_PER_CONN && g_ss.running) {
        req_no++;
        char req[STREAM_REQ_MAX + 1];
        size_t rlen = 0;
        if (carry_len > 0) {
            memcpy(req, carry, carry_len);
            rlen = carry_len;
            req[rlen] = '\0';
            carry_len = 0;
        }
        int got_error = 0;
        if (rlen == 0 || strstr(req, "\r\n\r\n") == NULL) {
            while (rlen < STREAM_REQ_MAX) {
                ssize_t n = recv(conn, req + rlen, STREAM_REQ_MAX - rlen, 0);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    got_error = 1;
                    if (req_no == 1 || rlen > 0) {
                        install_log("[STREAM] conn #%d peer=%s req=%d recv error after %zu bytes: %s (elapsed=%llums)",
                                    conn_id, peer, req_no, rlen, strerror(errno),
                                    (unsigned long long)(stream_now_ms() - t_start_ms));
                    }
                    break;
                }
                if (n == 0) {
                    break;
                }
                rlen += (size_t)n;
                req[rlen] = '\0';
                if (strstr(req, "\r\n\r\n") != NULL) {
                    break;
                }
            }
        }
        if (rlen == 0) {
            if (req_no == 1) {
                stream_debug_log("[STREAM] conn #%d peer=%s closed before sending headers (elapsed=%llums)",
                                 conn_id, peer,
                                 (unsigned long long)(stream_now_ms() - t_start_ms));
            } else if (!got_error) {
                stream_debug_log("[STREAM] conn #%d peer=%s keep-alive idle close after %d reqs (elapsed=%llums)",
                                 conn_id, peer, req_no - 1,
                                 (unsigned long long)(stream_now_ms() - t_start_ms));
            }
            break;
        }
        if (got_error) {
            break;
        }
        req[rlen] = '\0';

        /* Split pipelined bytes (if any) for the next iteration. */
        int overflow = 0;
        {
            char *hend = strstr(req, "\r\n\r\n");
            if (hend != NULL) {
                size_t hdr_len = (size_t)(hend + 4 - req);
                size_t left = rlen - hdr_len;
                if (left > sizeof(carry) - 1) {
                    left = 0; /* cannot happen; stay safe */
                } else if (left > 0) {
                    memcpy(carry, req + hdr_len, left);
                }
                carry_len = left;
                rlen = hdr_len;
                req[rlen] = '\0';
            } else {
                overflow = 1;
                install_log("[STREAM] conn #%d peer=%s req=%d WARNING: request exceeds %d bytes, truncating headers",
                            conn_id, peer, req_no, STREAM_REQ_MAX);
            }
        }

        serve_verdict_t v = serve_one_request(conn, conn_id, peer, t_start_ms, req_no, req, rlen, conn_smb);
        reqs_served++;
        if (overflow) {
            v = SERVE_CLOSE;
        }
        if (v == SERVE_CLOSE) {
            break;
        }
    }

    if (conn_smb != NULL) {
        smb_file_session_close(conn_smb);
        conn_smb = NULL;
    }

    stream_debug_log("[STREAM] conn #%d peer=%s finished reqs=%d (elapsed=%llums)",
                     conn_id, peer, reqs_served,
                     (unsigned long long)(stream_now_ms() - t_start_ms));
    stream_debug_log_conn_close(conn_id, peer, reqs_served);
    __sync_sub_and_fetch(&g_stream_active_workers, 1);
}

static serve_verdict_t serve_one_request(int conn, int conn_id, const char *peer,
                                         uint64_t t_start_ms, int req_no,
                                         char *req, size_t rlen,
                                         smb_file_session_t *conn_smb) {

    char line[512];
    size_t li = 0;
    while (li < rlen && li + 1 < sizeof(line) && req[li] != '\r' && req[li] != '\n') {
        line[li] = req[li];
        li++;
    }
    line[li] = '\0';
    stream_debug_log("[STREAM] conn #%d peer=%s req=%d request: '%s' (hdr_bytes=%zu elapsed=%llums)",
                     conn_id, peer, req_no, line, rlen,
                     (unsigned long long)(stream_now_ms() - t_start_ms));
    stream_debug_log("[STREAM] raw %s", line);

    /* Dump incoming header lines when debug stream logging is active so exotic
     * PS5 installer probes (User-Agent, Host, Connection, If-Range, ...) are visible. */
    if (stream_debug_enabled()) {
        const char *p = req;
        const char *first_eol = strstr(p, "\r\n");
        if (!first_eol) first_eol = strchr(p, '\n');
        p = first_eol ? ((*first_eol == '\r' && *(first_eol + 1) == '\n') ? first_eol + 2 : first_eol + 1) : p + strlen(p);
        int hidx = 0;
        while (*p && hidx < 64) {
            const char *eol = strstr(p, "\r\n");
            if (!eol) eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            if (eol == p) break; /* blank line = end of headers */
            {
                char hline[320];
                size_t hlen2 = (size_t)(eol - p);
                if (hlen2 >= sizeof(hline)) hlen2 = sizeof(hline) - 1;
                memcpy(hline, p, hlen2);
                hline[hlen2] = '\0';
                stream_debug_log("[STREAM] conn #%d peer=%s hdr[%d]: '%s'", conn_id, peer, hidx, hline);
            }
            hidx++;
            if (*eol == '\0') break;
            p = (*eol == '\r' && *(eol + 1) == '\n') ? eol + 2 : eol + 1;
            if (p[0] == '\r' || p[0] == '\n' || p[0] == '\0') break;
        }
        if (hidx == 0) {
            stream_debug_log("[STREAM] conn #%d peer=%s hdr: <none>", conn_id, peer);
        }
        /* One-line summary of the headers that matter most for range debugging. */
        {
            char hv_range[160] = {0}, hv_host[160] = {0}, hv_ua[200] = {0}, hv_conn[96] = {0};
            char hv_ir[160] = {0}, hv_cc[160] = {0};
            int has_range = stream_header_value(req, "Range", hv_range, sizeof(hv_range));
            int has_host = stream_header_value(req, "Host", hv_host, sizeof(hv_host));
            int has_ua = stream_header_value(req, "User-Agent", hv_ua, sizeof(hv_ua));
            int has_conn = stream_header_value(req, "Connection", hv_conn, sizeof(hv_conn));
            int has_ir = stream_header_value(req, "If-Range", hv_ir, sizeof(hv_ir));
            int has_cc = stream_header_value(req, "Cache-Control", hv_cc, sizeof(hv_cc));
            stream_debug_log("[STREAM] conn #%d peer=%s summary: Range='%s' Host='%s' UA='%s' Conn='%s' If-Range='%s' Cache='%s'",
                             conn_id, peer,
                             has_range ? hv_range : "<absent>",
                             has_host ? hv_host : "<absent>",
                             has_ua ? hv_ua : "<absent>",
                             has_conn ? hv_conn : "<absent>",
                             has_ir ? hv_ir : "<absent>",
                             has_cc ? hv_cc : "<absent>");
        }
    }

    /* Validate method + path: only GET/HEAD for a *.pkg under /stream/
     * are served. Anything else gets 404/405 instead of a PKG dump
     * (previously any request, even "HEAD /", returned the package body).
     * Strict-404: companion sidecars the client derives from the
     * content_id (e.g. <content_id>.crc) must 404 when we don't have
     * that file — serving PKG bytes as 206 poisons chunk validation
     * and aborts the install. */
    char method[16] = {0};
    char path[512] = {0};
    char version[16] = {0};
    sscanf(line, "%15s %511s %15s", method, path, version);
    int is_head = (strcmp(method, "HEAD") == 0);
    int is_get = (strcmp(method, "GET") == 0);
    /* Strip "?..." query for routing (ShellCore appends
     * ?product=...&serverIpAddr=...&r=...); keep raw path in logs. */
    char route[512] = {0};
    strncpy(route, path, sizeof(route) - 1);
    {
        char *qm = strchr(route, '?');
        if (qm) *qm = '\0';
    }
    /* Keep-alive basis (RFC 9112 §9.3): HTTP/1.1 defaults to persistent
     * unless "close" is sent; honour explicit "keep-alive" too. */
    int keep_wanted = 0;
    const char *conn_tok = "close";
    {
        char hv_conn[96] = {0};
        int has_conn = stream_header_value(req, "Connection", hv_conn, sizeof(hv_conn));
        int wants_close = has_conn && strcasestr(hv_conn, "close") != NULL;
        int wants_keep = has_conn && strcasestr(hv_conn, "keep-alive") != NULL;
        int http11 = (strcmp(version, "HTTP/1.1") == 0);
        keep_wanted = !wants_close && (wants_keep || http11);
        if (keep_wanted) {
            conn_tok = "keep-alive";
        }
    }
    stream_debug_log("[STREAM] conn #%d peer=%s req=%d parsed: method='%s' path='%.200s' version='%s' is_get=%d is_head=%d ka=%d",
                     conn_id, peer, req_no, method, path, version, is_get, is_head, keep_wanted);
    if (!is_get && !is_head) {
        char m405[256];
        int m405len = snprintf(m405, sizeof(m405),
                               "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
                               conn_tok);
        int sres = (m405len > 0) ? send_all(conn, m405, strlen(m405)) : -1;
        install_log("[STREAM] conn #%d peer=%s req=%d respond: 405 Method Not Allowed for '%s' (send=%s elapsed=%llums)",
                    conn_id, peer, req_no, method, sres == 0 ? "ok" : strerror(errno),
                    (unsigned long long)(stream_now_ms() - t_start_ms));
        install_log("[STREAM] WARNING: raw 405 Method Not Allowed for '%s'", method);
        stream_debug_log_request(conn_id, req_no, peer, method, path, 0, 0, 0, 405, 0, 0);
        return (sres == 0 && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
    }
    if (strncmp(route, "/stream/", 8) != 0) {
        char m404[256];
        int m404len = snprintf(m404, sizeof(m404),
                               "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
                               conn_tok);
        int sres = (m404len > 0) ? send_all(conn, m404, strlen(m404)) : -1;
        install_log("[STREAM] conn #%d peer=%s req=%d respond: 404 Not Found for '%.200s' (send=%s elapsed=%llums)",
                    conn_id, peer, req_no, path, sres == 0 ? "ok" : strerror(errno),
                    (unsigned long long)(stream_now_ms() - t_start_ms));
        install_log("[STREAM] WARNING: raw 404 Not Found for '%s'", path);
        stream_debug_log_request(conn_id, req_no, peer, method, path, 0, 0, 0, 404, 0, 0);
        return (sres == 0 && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
    }
    {
        /* Only the session *.pkg is served; sidecars (e.g. <content_id>.crc)
         * and foreign names 404. With a pinned session name the match must
         * be exact; otherwise (legacy) any *.pkg basename is served. */
        const char *slash = strrchr(route, '/');
        const char *base = slash ? slash + 1 : route;
        size_t blen = strlen(base);
        int is_pkg = (blen > 4 &&
                      base[blen - 4] == '.' &&
                      (base[blen - 3] == 'p' || base[blen - 3] == 'P') &&
                      (base[blen - 2] == 'k' || base[blen - 2] == 'K') &&
                      (base[blen - 1] == 'g' || base[blen - 1] == 'G'));
        char pinned[sizeof(g_ss.session_name)];
        pthread_mutex_lock(&g_ss.mutex);
        strncpy(pinned, g_ss.session_name, sizeof(pinned) - 1);
        pinned[sizeof(pinned) - 1] = '\0';
        pthread_mutex_unlock(&g_ss.mutex);
        if (pinned[0] != '\0') {
            if (strcmp(base, pinned) != 0) {
                char m404[256];
                int m404len = snprintf(m404, sizeof(m404),
                                       "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
                                       conn_tok);
                int sres = (m404len > 0) ? send_all(conn, m404, strlen(m404)) : -1;
                install_log("[STREAM] conn #%d peer=%s req=%d respond: 404 wrong-name for '%.200s' (expected='%.100s' send=%s elapsed=%llums)",
                            conn_id, peer, req_no, path, pinned, sres == 0 ? "ok" : strerror(errno),
                            (unsigned long long)(stream_now_ms() - t_start_ms));
                install_log("[STREAM] WARNING: raw 404 Not Found for '%s'", path);
                stream_debug_log_request(conn_id, req_no, peer, method, path, 0, 0, 0, 404, 0, 0);
                return (sres == 0 && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
            }
        } else if (!is_pkg) {
            char m404[256];
            int m404len = snprintf(m404, sizeof(m404),
                                   "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n",
                                   conn_tok);
            int sres = (m404len > 0) ? send_all(conn, m404, strlen(m404)) : -1;
            install_log("[STREAM] conn #%d peer=%s req=%d respond: 404 sidecar (no such file) for '%.200s' (send=%s elapsed=%llums)",
                        conn_id, peer, req_no, path, sres == 0 ? "ok" : strerror(errno),
                        (unsigned long long)(stream_now_ms() - t_start_ms));
            install_log("[STREAM] WARNING: raw 404 Not Found for '%s'", path);
            stream_debug_log_request(conn_id, req_no, peer, method, path, 0, 0, 0, 404, 0, 0);
            return (sres == 0 && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
        }
    }

    uint64_t total;
    pthread_mutex_lock(&g_ss.mutex);
    total = g_ss.total_size;
    pthread_mutex_unlock(&g_ss.mutex);

    uint64_t start = 0;
    uint64_t end = (total > 0) ? (total - 1) : 0;
    int rr = find_range(req, total, &start, &end);
    if (stream_debug_enabled()) {
        char raw_range[160] = {0};
        int has_raw = stream_header_value(req, "Range", raw_range, sizeof(raw_range));
        stream_debug_log("[STREAM] conn #%d peer=%s req=%d range: raw='%s' parsed=%d start=%llu end=%llu total=%llu",
                         conn_id, peer, req_no, has_raw ? raw_range : "<absent>", rr,
                         (unsigned long long)start, (unsigned long long)end,
                         (unsigned long long)total);
    }
    if (rr < 0) {
        char h416[256];
        int hlen = snprintf(h416, sizeof(h416),
                            "HTTP/1.1 416 Range Not Satisfiable\r\n"
                            "Content-Range: bytes */%llu\r\n"
                            "Accept-Ranges: bytes\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: %s\r\n"
                            "\r\n",
                            (unsigned long long)total, conn_tok);
        int sres = -1;
        if (hlen > 0) {
            sres = send_all(conn, h416, (size_t)hlen);
        }
        install_log("[STREAM] conn #%d peer=%s req=%d respond: 416 Range Not Satisfiable (total=%llu send=%s elapsed=%llums)",
                    conn_id, peer, req_no, (unsigned long long)total, sres == 0 ? "ok" : strerror(errno),
                    (unsigned long long)(stream_now_ms() - t_start_ms));
        install_log("[STREAM] WARNING: raw 416 Range Not Satisfiable (total=%llu)", (unsigned long long)total);
        stream_debug_log_request(conn_id, req_no, peer, method, path, 1, start, end, 416, 0, total);
        return (sres == 0 && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
    }

    int is_range = (rr == 1);
    uint64_t content_length = (total == 0) ? 0 : (end - start + 1);

    /* Validators: stock servers send Date + Last-Modified; ShellCore's
       downloader advances chunks only against cacheable responses. The
       Last-Modified value is intentionally fixed so it is stable across
       requests, runs, and part layouts. */
    char datestr[64];
    datestr[0] = '\0';
    {
        time_t now = time(NULL);
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        if (gmtime_r(&now, &tmv) != NULL) {
            strftime(datestr, sizeof(datestr), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
        }
    }

    char hdr[1024];
    int hlen;
    if (is_range) {
        hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Date: %s\r\n"
                        "Last-Modified: Wed, 01 Jan 2025 00:00:00 GMT\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Length: %llu\r\n"
                        "Content-Range: bytes %llu-%llu/%llu\r\n"
                        "Content-Disposition: attachment; filename=\"package.pkg\"\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        datestr,
                        (unsigned long long)content_length,
                        (unsigned long long)start, (unsigned long long)end,
                        (unsigned long long)total, conn_tok);
    } else {
        hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Date: %s\r\n"
                        "Last-Modified: Wed, 01 Jan 2025 00:00:00 GMT\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Length: %llu\r\n"
                        "Content-Disposition: attachment; filename=\"package.pkg\"\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        datestr,
                        (unsigned long long)total, conn_tok);
    }
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        install_log("[STREAM] conn #%d peer=%s req=%d ERROR: header build failed (hlen=%d is_range=%d)",
                    conn_id, peer, req_no, hlen, is_range);
        return SERVE_CLOSE;
    }
    if (is_range) {
        stream_debug_log("[STREAM] conn #%d peer=%s req=%d respond: 206 Partial Content len=%llu range=%llu-%llu/%llu head=%d ka=%d",
                         conn_id, peer, req_no, (unsigned long long)content_length,
                         (unsigned long long)start, (unsigned long long)end,
                         (unsigned long long)total, is_head, keep_wanted);
    } else {
        stream_debug_log("[STREAM] conn #%d peer=%s req=%d respond: 200 OK len=%llu head=%d ka=%d (no Range header)",
                         conn_id, peer, req_no, (unsigned long long)total, is_head, keep_wanted);
    }
    stream_debug_log_request(conn_id, req_no, peer, method, path,
                             is_range, start, end,
                             is_range ? 206 : 200, content_length, total);
    if (send_all(conn, hdr, (size_t)hlen) != 0) {
        install_log("[STREAM] conn #%d peer=%s req=%d ERROR: header send failed: %s (elapsed=%llums)",
                    conn_id, peer, req_no, strerror(errno),
                    (unsigned long long)(stream_now_ms() - t_start_ms));
        return SERVE_CLOSE;
    }
    stream_debug_log("[STREAM] conn #%d peer=%s req=%d headers sent (%d bytes, elapsed=%llums)",
                     conn_id, peer, req_no, hlen,
                     (unsigned long long)(stream_now_ms() - t_start_ms));

    /* HEAD: headers only, no body and no progress accounting. */
    if (is_head) {
        stream_debug_log("[STREAM] conn #%d peer=%s req=%d done: HEAD complete, no body (elapsed=%llums)",
                         conn_id, peer, req_no,
                         (unsigned long long)(stream_now_ms() - t_start_ms));
        return keep_wanted ? SERVE_KEEP : SERVE_CLOSE;
    }

    char *sbuf = (char *)malloc(STREAM_SEND_CHUNK);
    if (!sbuf) {
        install_log("[STREAM] conn #%d peer=%s req=%d ERROR: body buffer malloc failed", conn_id, peer, req_no);
        return SERVE_CLOSE;
    }
    stream_debug_log("[STREAM] conn #%d peer=%s body start: off=%llu end=%llu bytes=%llu",
                     conn_id, peer, (unsigned long long)start, (unsigned long long)end,
                     (unsigned long long)content_length);
    uint64_t off = start;
    uint64_t sent_total = 0;
    const char *end_reason = "complete";
    /* Pin the stream for the whole body: local preads run lock-free
     * (virtual_stream_t carries its own state lock; SMB sessions
     * self-serialize), and stop() waits for this ref before freeing. */
    virtual_stream_t *vp = stream_vs_acquire();
    if (vp == NULL) {
        end_reason = "server-stopping";
    }
    while (vp != NULL && off <= end && g_ss.running) {
        uint64_t remain = end - off + 1;
        size_t want = (remain > STREAM_SEND_CHUNK) ? STREAM_SEND_CHUNK : (size_t)remain;
        ssize_t n;
        if (conn_smb != NULL) {
            n = smb_file_session_read(conn_smb, sbuf, want, off);
        } else {
            n = virtual_stream_read(vp, off, sbuf, want);
        }
        if (n <= 0) {
            end_reason = "read-short";
            install_log("[STREAM] conn #%d peer=%s body read short at off=%llu want=%zu (res: %zd reason=%s)",
                        conn_id, peer, (unsigned long long)off, want, n, end_reason);
            install_log("[STREAM] WARNING: raw read short at %llu (res: %zd)", (unsigned long long)off, n);
            break;
        }
        if (send_all(conn, sbuf, (size_t)n) != 0) {
            end_reason = "client-disconnect";
            install_log("[STREAM] conn #%d peer=%s body send failed at off=%llu n=%zd sent=%llu/%llu: %s (elapsed=%llums)",
                        conn_id, peer, (unsigned long long)off, n,
                        (unsigned long long)sent_total, (unsigned long long)content_length,
                        strerror(errno),
                        (unsigned long long)(stream_now_ms() - t_start_ms));
            break;
        }
        installer_notify_bytes_streamed((uint64_t)n);
        sent_total += (uint64_t)n;
        if (stream_debug_enabled()) {
            if ((off % (25 * 1024 * 1024)) < (uint64_t)n) {
                pthread_mutex_lock(&g_ss.mutex);
                uint64_t tot = g_ss.total_size;
                pthread_mutex_unlock(&g_ss.mutex);
                stream_debug_log("[HTTP] Stream read offset: %llu / %llu (%.1f%%)",
                                 (unsigned long long)(off + (uint64_t)n), (unsigned long long)tot,
                                 tot > 0 ? ((float)(off + (uint64_t)n) / (float)tot) * 100.0f : 0.0f);
            }
            /* Per-request progress at 25MB granularity keeps /api/log readable
             * while still showing stalls on large PKGs. */
            if ((sent_total % (25 * 1024 * 1024)) < (uint64_t)n || sent_total == content_length) {
                uint64_t el = stream_now_ms() - t_start_ms;
                unsigned long long kbps = el > 0 ? (sent_total / 1024 * 1000ULL / el) : 0ULL;
                stream_debug_log("[STREAM] conn #%d peer=%s body progress: sent=%llu/%llu (%.1f%%) off=%llu elapsed=%llums %lluKB/s",
                                 conn_id, peer, (unsigned long long)sent_total,
                                 (unsigned long long)content_length,
                                 content_length > 0 ? ((float)sent_total / (float)content_length) * 100.0f : 0.0f,
                                 (unsigned long long)(off + (uint64_t)n),
                                 (unsigned long long)el, kbps);
            }
        }
        off += (uint64_t)n;
    }
    int complete = (off > end);
    if (complete) {
        end_reason = "complete";
    } else if (g_ss.running == 0 && end_reason[0] == 'c' && sent_total != content_length) {
        /* Preserve "complete" when the loop exited exactly at end even as
         * the server is stopping; otherwise report the stop. */
        if (sent_total != content_length) {
            end_reason = "server-stopping";
        }
    }
    free(sbuf);

    if (stream_debug_enabled()) {
        uint64_t el = stream_now_ms() - t_start_ms;
        unsigned long long kbps = el > 0 ? (sent_total / 1024 * 1000ULL / el) : 0ULL;
        stream_debug_log("[STREAM] conn #%d peer=%s req=%d done: reason=%s sent=%llu/%llu off=%llu range=%llu-%llu/%llu elapsed=%llums %lluKB/s",
                         conn_id, peer, req_no, end_reason,
                         (unsigned long long)sent_total, (unsigned long long)content_length,
                         (unsigned long long)off,
                         (unsigned long long)start, (unsigned long long)end,
                         (unsigned long long)total,
                         (unsigned long long)el, kbps);
    }
    stream_debug_log_response_done(conn_id, req_no, peer, sent_total, content_length, end_reason);

    if (vp != NULL) {
        stream_vs_release();
    }
    return (complete && keep_wanted) ? SERVE_KEEP : SERVE_CLOSE;
}

static void *connection_worker(void *arg) {
    stream_conn_arg_t *ca = (stream_conn_arg_t *)arg;
    int conn = -1;
    int conn_id = 0;
    char peer[64] = {0};
    if (!ca) {
        return NULL;
    }
    conn = ca->conn;
    conn_id = ca->conn_id;
    strncpy(peer, ca->peer, sizeof(peer) - 1);
    free(ca);
    stream_track_add(conn);
    serve_connection(conn, conn_id, peer);
    stream_track_remove(conn);
    close(conn);
    stream_debug_log("[STREAM] conn #%d peer=%s socket closed (fd=%d workers=%d)",
                     conn_id, peer, conn, (int)g_stream_active_workers);
    return NULL;
}

static void *listener_fn(void *arg) {
    (void)arg;
    while (g_ss.running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int conn = accept(g_ss.listen_fd, (struct sockaddr *)&cli, &cl);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!g_ss.running) {
                break;
            }
            install_log("[STREAM] accept failed: %s", strerror(errno));
            usleep(10000);
            continue;
        }
        int conn_id = __sync_add_and_fetch(&g_stream_conn_seq, 1);
        char peer[64];
        stream_format_peer(&cli, peer, sizeof(peer));
        stream_debug_log_conn_open(conn_id, peer);
        uint64_t total_snapshot;
        pthread_mutex_lock(&g_ss.mutex);
        total_snapshot = g_ss.total_size;
        pthread_mutex_unlock(&g_ss.mutex);
        stream_debug_log("[STREAM] conn #%d accepted from %s (fd=%d workers=%d total=%llu)",
                         conn_id, peer, conn, (int)g_stream_active_workers,
                         (unsigned long long)total_snapshot);
        /* Only loopback clients are expected (bound to 127.0.0.1), but
         * never let one slow/hung connection starve the installer:
         * hand each connection to a detached worker so the listener
         * keeps accepting. Local preads run lock-free; SMB sessions
         * serialize internally. */
        stream_conn_arg_t *ca = (stream_conn_arg_t *)malloc(sizeof(*ca));
        if (!ca) {
            install_log("[STREAM] conn #%d peer=%s ERROR: worker alloc failed, serving inline",
                        conn_id, peer);
            stream_track_add(conn);
            serve_connection(conn, conn_id, peer);
            stream_track_remove(conn);
            close(conn);
            stream_debug_log("[STREAM] conn #%d peer=%s socket closed (fd=%d workers=%d)",
                             conn_id, peer, conn, (int)g_stream_active_workers);
            continue;
        }
        ca->conn = conn;
        ca->conn_id = conn_id;
        strncpy(ca->peer, peer, sizeof(ca->peer) - 1);
        ca->peer[sizeof(ca->peer) - 1] = '\0';
        pthread_t wth;
        if (pthread_create(&wth, NULL, connection_worker, ca) == 0) {
            pthread_detach(wth);
        } else {
            install_log("[STREAM] conn #%d peer=%s ERROR: worker spawn failed (%s), serving inline",
                        conn_id, peer, strerror(errno));
            stream_track_add(conn);
            serve_connection(conn, conn_id, peer);
            stream_track_remove(conn);
            close(conn);
            free(ca);
            stream_debug_log("[STREAM] conn #%d peer=%s socket closed (fd=%d workers=%d)",
                             conn_id, peer, conn, (int)g_stream_active_workers);
        }
    }
    return NULL;
}

int stream_server_session_start(const char *pkg_path) {
    return stream_server_session_start_ex(pkg_path, NULL);
}

int stream_server_session_start_ex(const char *pkg_path, const char *session_name) {
    if (!pkg_path || pkg_path[0] == '\0') {
        return -1;
    }
    if (g_ss.running) {
        stream_server_session_stop();
    }

    virtual_stream_t *vs = (virtual_stream_t *)calloc(1, sizeof(*vs));
    if (!vs) {
        return -1;
    }
    if (virtual_stream_open(pkg_path, vs) != 0) {
        free(vs);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        virtual_stream_close(vs);
        free(vs);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(STREAM_SERVER_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        install_log("[STREAM] ERROR: raw bind :%d failed: %s", STREAM_SERVER_PORT, strerror(errno));
        close(fd);
        virtual_stream_close(vs);
        free(vs);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        install_log("[STREAM] ERROR: raw listen failed: %s", strerror(errno));
        close(fd);
        virtual_stream_close(vs);
        free(vs);
        return -1;
    }

    pthread_mutex_lock(&g_ss.mutex);
    g_ss.vs = vs;
    g_ss.vs_refs = 0;
    g_ss.stopping = 0;
    g_ss.total_size = vs->total_pkg_size;
    g_ss.listen_fd = fd;
    g_ss.running = 1;
    if (!session_name || session_name[0] == '\0') {
        g_ss.session_name[0] = '\0';
    } else {
        const char *slash = strrchr(session_name, '/');
        const char *base = slash ? slash + 1 : session_name;
        strncpy(g_ss.session_name, base, sizeof(g_ss.session_name) - 1);
        g_ss.session_name[sizeof(g_ss.session_name) - 1] = '\0';
    }
    if (pthread_create(&g_ss.thread, NULL, listener_fn, NULL) != 0) {
        g_ss.running = 0;
        g_ss.listen_fd = -1;
        g_ss.vs = NULL;
        g_ss.total_size = 0;
        g_ss.session_name[0] = '\0';
        pthread_mutex_unlock(&g_ss.mutex);
        close(fd);
        virtual_stream_close(vs);
        free(vs);
        return -1;
    }
    g_ss.thread_created = 1;
    pthread_mutex_unlock(&g_ss.mutex);

    install_log("[STREAM] raw server listening on 127.0.0.1:%d (size: %llu bytes)",
                STREAM_SERVER_PORT, (unsigned long long)vs->total_pkg_size);
    return 0;
}

void stream_server_session_stop(void) {
    int join = 0;
    int lfd = -1;
    int kick[STREAM_MAX_TRACKED_FDS];
    int nkick = 0;

    pthread_mutex_lock(&g_ss.mutex);
    if (g_ss.running || g_ss.thread_created) {
        g_ss.running = 0;
        g_ss.stopping = 1;
        join = g_ss.thread_created;
        g_ss.thread_created = 0;
    }
    lfd = g_ss.listen_fd;
    g_ss.listen_fd = -1;
    for (int i = 0; i < STREAM_MAX_TRACKED_FDS && nkick < STREAM_MAX_TRACKED_FDS; i++) {
        if (g_ss.live_fds[i] >= 0) {
            kick[nkick++] = g_ss.live_fds[i];
            g_ss.live_fds[i] = -1;
        }
    }
    pthread_mutex_unlock(&g_ss.mutex);

    /* shutdown() only (never close()): each worker/listener owns its fd
     * and close()s it after use. close() here would race and could close
     * an unrelated reused fd (MHD/SMB socket). Shutting everything down
     * unblocks recv/send/keep-alive-idle workers so they exit promptly. */
    for (int i = 0; i < nkick; i++) {
        shutdown(kick[i], SHUT_RDWR);
    }
    if (lfd >= 0) {
        shutdown(lfd, SHUT_RDWR);
        close(lfd);
    }
    if (join) {
        pthread_join(g_ss.thread, NULL);
    }

    /* Wait for in-flight request bodies to drop their stream refs before
     * freeing: with lock-free preads a worker may still be transferring.
     * Refs always drain: transfers abort on the shutdown above, and the
     * disc waiter breaks on cancel/shutdown flags. */
    pthread_mutex_lock(&g_ss.mutex);
    while (g_ss.vs_refs > 0) {
        pthread_cond_wait(&g_ss.vs_cond, &g_ss.mutex);
    }
    if (g_ss.vs) {
        virtual_stream_close(g_ss.vs);
        free(g_ss.vs);
        g_ss.vs = NULL;
    }
    g_ss.stopping = 0;
    g_ss.total_size = 0;
    g_ss.session_name[0] = '\0';
    pthread_mutex_unlock(&g_ss.mutex);
    stream_debug_log_close();
    install_log("[STREAM] raw server stopped");
}
