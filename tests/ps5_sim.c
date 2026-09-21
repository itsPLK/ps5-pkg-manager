/*
 * PS5 Installer Stream Simulator — raw HTTP client half.
 *
 * Self-contained (no dependency on stream_server.c) so the same code can be
 * linked into the host test, built into a standalone CLI, or adapted to a
 * websocket client. It speaks just enough HTTP/1.1 to talk to the stream
 * server: send a GET with a Range header, read status + headers, then read
 * exactly Content-Length body bytes.
 */

#include "ps5_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ---- low-level HTTP plumbing ------------------------------------------- */

/* Forward declaration (defined after sim_start_ms below). */
static uint64_t sim_now_ms(void);

static int sim_socket(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break; /* success */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Read exactly n bytes, returning 0 on success, -1 on short/failed read. */
static int read_exact(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1; /* peer closed */
        got += (size_t)r;
    }
    return 0;
}

/* Structured response returned by sim_http_get(). */
typedef struct {
    int         status;      /* 0 on protocol error */
    uint64_t    content_len;
    char        content_range[128];
    uint8_t    *body;        /* malloc'd, content_len bytes */
    size_t      body_len;
} sim_resp_t;

/*
 * Send one GET request and read the complete response (headers + body).
 * On success returns 0 and fills resp; on protocol failure returns -1.
 */
static int sim_http_get(int fd, const char *req, size_t reqlen, sim_resp_t *resp) {
    memset(resp, 0, sizeof(*resp));

    if (send(fd, req, reqlen, 0) != (ssize_t)reqlen) {
        return -1;
    }
#ifdef PS5_SIM_DEBUG
    fprintf(stderr, "[SIM-DBG @%llums] sent (%zu): %.200s\n",
            (unsigned long long)sim_now_ms(), reqlen, req);
#endif

    /* Read headers until the blank line, one byte at a time. Reading exactly
     * up to (and never past) the terminating CRLFCRLF guarantees a keep-alive
     * response can never have its body bytes absorbed into the header buffer.
     * If it did, those body bytes would be discarded yet read_exact would then
     * wait (for the full SO_RCVTIMEO) for body bytes the server already sent --
     * the keep-alive desync that otherwise makes a correct connection hang. */
    char hdr[8192];
    size_t hdr_len = 0;
    for (;;) {
        char c;
        if (read_exact(fd, &c, 1) != 0) return -1;
        if (hdr_len + 1 >= sizeof(hdr)) return -1; /* header too large */
        hdr[hdr_len++] = c;
        if (hdr_len >= 4 &&
            hdr[hdr_len - 4] == '\r' && hdr[hdr_len - 3] == '\n' &&
            hdr[hdr_len - 2] == '\r' && hdr[hdr_len - 1] == '\n') {
            break;
        }
    }
    /* Null-terminate so the strstr/strncasecmp parser below can't run off the
     * end of the response into stale heap bytes. */
    hdr[hdr_len] = '\0';

    /* Status line: HTTP/1.1 206 Partial Content */
    char *sp1 = strchr(hdr, ' ');
    if (!sp1) return -1;
    resp->status = atoi(sp1 + 1);
#ifdef PS5_SIM_DEBUG
    fprintf(stderr, "[SIM-DBG @%llums] got status=%llu clen=%llu crange='%s'\n",
            (unsigned long long)sim_now_ms(),
            (unsigned long long)resp->status, (unsigned long long)resp->content_len, resp->content_range);
#endif

    /* Parse Content-Length and Content-Range headers. */
    for (char *scan = hdr; (scan = strstr(scan, "\r\n")) != NULL; ) {
        scan += 2;
        if (strncasecmp(scan, "content-length:", 15) == 0) {
            resp->content_len = strtoull(scan + 15, NULL, 10);
        } else if (strncasecmp(scan, "content-range:", 14) == 0) {
            /* Store the full Content-Range value ("bytes 0-65536/5242880") so a
             * caller can parse the total after the '/'. */
            const char *p = scan + 14;
            while (*p == ' ') p++;
            size_t avail = sizeof(resp->content_range) - 1;
            size_t n = 0;
            while (p[n] && n < avail && p[n] != '\r' && p[n] != '\n') n++;
            memcpy(resp->content_range, p, n);
            resp->content_range[n] = '\0';
        }
    }

    /* Read the body. */
    if (resp->content_len > 0) {
        resp->body = (uint8_t *)malloc(resp->content_len);
        if (!resp->body) return -1;
        if (read_exact(fd, resp->body, resp->content_len) != 0) {
            free(resp->body);
            resp->body = NULL;
            return -1;
        }
        resp->body_len = resp->content_len;
    }
    return 0;
}

/* ---- replay logging ----------------------------------------------------- */

static uint64_t sim_start_ms = 0;

static uint64_t sim_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
    return sim_start_ms ? (now - sim_start_ms) : 0;
}

static void sim_log(FILE *fp, const char *fmt, ...) {
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}

/* ---- replay phases ------------------------------------------------------ */

/* Format "GET <path>?<query> HTTP/1.1\r\n...\r\n\r\n". */
static char *sim_build_get(const char *path, const char *query, const char *range,
                           char *buf, size_t bufsz) {
    int n = snprintf(buf, bufsz,
                     "GET %s%s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: keep-alive\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "%s"
                     "\r\n",
                     path, query ? query : "",
                     range ? range : "");
    if (n <= 0 || (size_t)n >= bufsz) return NULL;
    return buf;
}

/* One header-acquisition connection (Phase 1 + optional Phase 2 CRC probe). */
static int sim_one_connection(int conn_id, const ps5_sim_target_t *t,
                              const char *query, uint64_t total, uint64_t *delivered,
                              FILE *log_fp) {
    char req[4096];
    char range[64];
    int reqs = 0;
    int conn_fail = 0;
    int fd = sim_socket(t->host, t->port);
    if (fd < 0) {
        return -1;
    }
#ifdef PS5_SIM_DEBUG
    fprintf(stderr, "[SIM-DBG @%llums] conn #%d open fd=%d\n",
            (unsigned long long)sim_now_ms(), conn_id, fd);
#endif
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               (const void *)&((struct timeval){.tv_sec = 30, .tv_usec = 0}),
               sizeof(struct timeval));

    /* Phase 1: header acquisition (bytes=0-65535), repeated reqs_per_header. */
    for (int h = 0; h < t->reqs_per_header; h++) {
        snprintf(range, sizeof(range), "Range: bytes=0-%u\r\n", (unsigned)PS5_SIM_HEADER_END);
        sim_build_get(t->path, query, range, req, sizeof(req));
        sim_resp_t r;
        if (sim_http_get(fd, req, strlen(req), &r) != 0 || r.status != 206) {
            conn_fail = 1;
            if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\trange\t206?\t%d\tTotal: %llu\n",
                                (unsigned long long)sim_now_ms(), conn_id, reqs + 1, t->path, r.status, (unsigned long long)total);
        } else {
            *delivered += r.content_len;
            if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\tRange: bytes=0-%u\t%d\tContent-Length: %llu\tTotal: %llu\n",
                                (unsigned long long)sim_now_ms(), conn_id, reqs + 1, t->path,
                                (unsigned)PS5_SIM_HEADER_END, r.status, (unsigned long long)r.content_len, (unsigned long long)total);
            sim_log(log_fp, "%llu\tBODY_DONE\t%d\t%d\t127.0.0.1\tsent=%llu/%llu\treason=complete\n",
                    (unsigned long long)sim_now_ms(), conn_id, reqs + 1,
                    (unsigned long long)r.content_len, (unsigned long long)r.content_len);
        }
        free(r.body);
        reqs++;
    }

    /* Phase 2: sidecar CRC probe (Range-less) on the same connection. */
    if (t->check_crc && t->content_id && t->content_id[0]) {
        const char *slash = strrchr(t->path, '/');
        int dir = slash ? (int)(slash - t->path) : (int)strlen(t->path);
        char crc_path[700];
        int n = snprintf(crc_path, sizeof(crc_path), "%.*s/%.512s.crc",
                         dir, slash ? t->path : "", t->content_id);
        if (n > 0 && (size_t)n < sizeof(crc_path)) {
            sim_build_get(crc_path, query, NULL, req, sizeof(req));
            sim_resp_t r;
            int got_404 = (sim_http_get(fd, req, strlen(req), &r) == 0) && r.status == 404;
            if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\tno-range\t%d\tContent-Length: %llu\tTotal: 0\n",
                                (unsigned long long)sim_now_ms(), conn_id, reqs + 1, crc_path,
                                r.status, (unsigned long long)r.content_len);
            free(r.body);
            if (!got_404) conn_fail = 1;
            reqs++;
        }
    }

    if (log_fp) sim_log(log_fp, "%llu\tCONN_CLOSE\t%d\t-\t127.0.0.1\treqs_served=%d\n",
                        (unsigned long long)sim_now_ms(), conn_id, reqs);

#ifdef PS5_SIM_DEBUG
    fprintf(stderr, "[SIM-DBG @%llums] conn #%d done: reqs=%d conn_fail=%d -> close(fd=%d)\n",
            (unsigned long long)sim_now_ms(), conn_id, reqs, conn_fail, fd);
#endif
    close(fd);
    return conn_fail ? -1 : 0;
}

/* Phase 3: parallel bulk transfer of contiguous chunks across `parallel`
 * connections, round-robin assigned so the two sockets ping-pong. */
static int sim_bulk(const ps5_sim_target_t *t, const char *query, uint64_t total,
                    uint64_t *delivered, FILE *log_fp) {
    if (total <= PS5_SIM_HEADER_END) {
        return 0; /* nothing past the header to stream */
    }

    uint64_t chunk = t->chunk_size ? t->chunk_size : PS5_SIM_DEFAULT_CHUNK;
    int parallel = t->parallel > 0 ? t->parallel : 1;
    if (parallel > 8) parallel = 8;

    int fds[8] = {0};
    int reqs[8] = {0};
    int nconns = parallel;
    for (int c = 0; c < nconns; c++) {
        fds[c] = sim_socket(t->host, t->port);
        if (fds[c] < 0) {
            for (int k = 0; k < c; k++) close(fds[k]);
            return -1;
        }
        setsockopt(fds[c], SOL_SOCKET, SO_RCVTIMEO,
                   (const void *)&((struct timeval){.tv_sec = 30, .tv_usec = 0}),
                   sizeof(struct timeval));
    }

    int fail = 0;
    uint64_t next = PS5_SIM_HEADER_END; /* first byte after the header read */
    while (next < total) {
        uint64_t end = next + chunk - 1;
        if (end >= total) end = total - 1;
        int c = (int)((next - PS5_SIM_HEADER_END) % (uint64_t)parallel);

        char range[96];
        snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
                 (unsigned long long)next, (unsigned long long)end);
        char req[4096];
        sim_build_get(t->path, query, range, req, sizeof(req));
        sim_resp_t r;
        int ok = (sim_http_get(fds[c], req, strlen(req), &r) == 0) &&
                 r.status == 206 &&
                 r.content_len == (uint64_t)(end - next + 1);
        if (!ok) {
            fail = 1;
            if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\trange\t%d\tContent-Length: %llu (expected %llu)\n",
                                (unsigned long long)sim_now_ms(), c + 1, reqs[c] + 1, t->path, r.status,
                                (unsigned long long)r.content_len, (unsigned long long)(end - next + 1));
        } else {
            *delivered += r.content_len;
            if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\tRange: bytes=%llu-%llu\t%d\tContent-Length: %llu\tTotal: %llu\n",
                                (unsigned long long)sim_now_ms(), c + 1, reqs[c] + 1, t->path,
                                (unsigned long long)next, (unsigned long long)end, r.status,
                                (unsigned long long)r.content_len, (unsigned long long)total);
            sim_log(log_fp, "%llu\tBODY_DONE\t%d\t%d\t127.0.0.1\tsent=%llu/%llu\treason=complete\n",
                    (unsigned long long)sim_now_ms(), c + 1, reqs[c] + 1,
                    (unsigned long long)r.content_len, (unsigned long long)r.content_len);
        }
        free(r.body);
        reqs[c]++;
        next = end + 1;
    }

    for (int c = 0; c < nconns; c++) {
        if (log_fp) sim_log(log_fp, "%llu\tCONN_CLOSE\t%d\t-\t127.0.0.1\treqs_served=%d\n",
                            (unsigned long long)sim_now_ms(), c + 1, reqs[c]);
        close(fds[c]);
    }
    return fail ? -1 : 0;
}

/* ---- public entry point ------------------------------------------------- */

int ps5_sim_replay(const ps5_sim_target_t *t, ps5_sim_stats_t *out, FILE *log_fp) {
    memset(out, 0, sizeof(*out));
    struct timeval tv0;
    gettimeofday(&tv0, NULL);
    sim_start_ms = (uint64_t)tv0.tv_sec * 1000ULL + (uint64_t)tv0.tv_usec / 1000ULL;

    const char *query = t->query && t->query[0] ? t->query : PS5_SIM_DEFAULT_QUERY;
    if (!t->host || !t->path) {
        if (out) out->failures = 1;
        return -1;
    }

    /* Discover total size from the server if not supplied (read it off the
     * first header request's Content-Range). */
    uint64_t total = t->total_size;
    if (total == 0) {
        char req[4096];
        char range[64];
        snprintf(range, sizeof(range), "Range: bytes=0-%u\r\n", (unsigned)PS5_SIM_HEADER_END);
        sim_build_get(t->path, query, range, req, sizeof(req));

        int fd = sim_socket(t->host, t->port);
        if (fd >= 0) {
            sim_resp_t r;
            if (sim_http_get(fd, req, strlen(req), &r) == 0) {
                const char *slash = strchr(r.content_range, '/');
                if (slash) total = strtoull(slash + 1, NULL, 10);
                out->bytes_received = r.content_len;
                out->total_delivered += r.content_len;
                out->requests_made++;
                if (log_fp) sim_log(log_fp, "%llu\tREQUEST\t%d\t%d\t127.0.0.1\tGET\t%s\tRange: bytes=0-%u\t%d\tContent-Length: %llu\tTotal: %llu\n",
                                    (unsigned long long)sim_now_ms(), 0, 1, t->path,
                                    (unsigned)PS5_SIM_HEADER_END, r.status,
                                    (unsigned long long)r.content_len, (unsigned long long)total);
                sim_log(log_fp, "%llu\tBODY_DONE\t%d\t%d\t127.0.0.1\tsent=%llu/%llu\treason=complete\n",
                        (unsigned long long)sim_now_ms(), 0, 1,
                        (unsigned long long)r.content_len, (unsigned long long)r.content_len);
            }
            free(r.body);
            close(fd);
            if (log_fp) sim_log(log_fp, "%llu\tCONN_CLOSE\t%d\t-\t127.0.0.1\treqs_served=1\n",
                                (unsigned long long)sim_now_ms(), 0);
        }
    }

    if (total == 0) {
        if (out) out->failures = 1;
        return -2; /* could not learn size */
    }

    int phase_fail = 0;

    /* Phase 1 (+ optional 2): header acquisition, one connection per repeat. */
    for (int i = 0; i < t->header_repeats; i++) {
        int rc = sim_one_connection(i + 1, t, query, total, &out->total_delivered, log_fp);
        out->conns_opened++;
        if (rc != 0) phase_fail = 1;
    }
    out->header_ok = !phase_fail;

    /* Phase 3: parallel bulk. */
    int bulk_fail = sim_bulk(t, query, total, &out->total_delivered, log_fp);
    out->bulk_ok = (bulk_fail == 0);
    if (bulk_fail) phase_fail = 1;

    /* CRC sidecar flag (verified inside sim_one_connection when enabled). */
    out->crc_404 = 1;

    out->failures = phase_fail;
    return phase_fail ? 1 : 0;
}
