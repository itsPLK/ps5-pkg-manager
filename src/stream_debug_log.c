/*
 * PKG Manager - Stream Debug File Logger
 *
 * Records every TCP connection and HTTP range request from the PS5 system
 * client to the stream server, producing a text log that can be used to
 * build an exact mock / replay of the download pattern for remote
 * (WebSocket) streaming development.
 */

#include "stream_debug_log.h"
#include "debug_log_retention.h"
#include "installer.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t g_dbglog_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_dbglog_fp = NULL;
static uint64_t g_dbglog_session_start_ms = 0;
static uint64_t g_dbglog_ws_bytes = 0;
static uint64_t g_dbglog_ws_pending_bytes = 0;
static uint64_t g_dbglog_ws_last_ms = 0;
static uint64_t g_dbglog_ws_last_segment = 0;
static uint64_t g_dbglog_rx_bytes = 0;
static uint64_t g_dbglog_rx_pending_bytes = 0;
static uint64_t g_dbglog_rx_active_us = 0;
static uint64_t g_dbglog_rx_last_ms = 0;
static uint64_t g_dbglog_rx_last_segment = 0;
static uint64_t g_dbglog_busy_attempts = 0;
static uint64_t g_dbglog_busy_pending_attempts = 0;
static uint64_t g_dbglog_busy_pending_bytes = 0;
static uint64_t g_dbglog_busy_last_ms = 0;
static uint64_t g_dbglog_busy_last_segment = 0;

static uint64_t g_cache_duplicate, g_cache_reload, g_cache_unread;
static uint64_t g_cache_last_ms;
static int g_cache_dirty;

static uint64_t g_sender_read_us, g_sender_ack_us, g_sender_acks, g_sender_sent;
static uint64_t g_sender_window, g_sender_last_ms;
static int g_sender_dirty;

static uint64_t dbglog_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

/* Elapsed milliseconds since session open, for relative timestamps. */
static uint64_t dbglog_elapsed_ms(void) {
    return dbglog_now_ms() - g_dbglog_session_start_ms;
}

static void dbglog_write(const char *line) {
    /* Caller must hold g_dbglog_mutex. */
    if (!g_dbglog_fp) return;
    fputs(line, g_dbglog_fp);
    fflush(g_dbglog_fp);
}

void stream_debug_log_event(const char *message) {
    if (!message) return;
    pthread_mutex_lock(&g_dbglog_mutex);
    if (g_dbglog_fp) {
        char clean[512];
        snprintf(clean, sizeof(clean), "%s", message);
        for (char *p = clean; *p; ++p) {
            if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        char line[600];
        snprintf(line, sizeof(line), "%llu\tINSTALL_EVENT\t-\t-\t-\t%s\n",
                 (unsigned long long)dbglog_elapsed_ms(), clean);
        dbglog_write(line);
    }
    pthread_mutex_unlock(&g_dbglog_mutex);
}

/* Caller holds g_dbglog_mutex. Upload events are batched to keep the debug
 * file useful without writing and flushing once for every 1 MiB segment. */
static void dbglog_write_ws_sample(uint64_t now) {
    if (!g_dbglog_fp || g_dbglog_ws_pending_bytes == 0) return;
    uint64_t interval = now - g_dbglog_ws_last_ms;
    double mbps = interval ? (double)g_dbglog_ws_pending_bytes /
                            ((double)interval * 1000.0) : 0.0;
    char line[384];
    snprintf(line, sizeof(line),
             "%llu\tWS_ACCEPT\t-\t-\tbytes=%llu\tcumulative=%llu\tinterval_ms=%llu\tMB/s=%.2f\tlast_segment=%llu\n",
             (unsigned long long)now,
             (unsigned long long)g_dbglog_ws_pending_bytes,
             (unsigned long long)g_dbglog_ws_bytes,
             (unsigned long long)interval, mbps,
             (unsigned long long)g_dbglog_ws_last_segment);
    dbglog_write(line);
    g_dbglog_ws_pending_bytes = 0;
    g_dbglog_ws_last_ms = now;
}

static void dbglog_write_rx_sample(uint64_t now) {
    if (!g_dbglog_fp || g_dbglog_rx_pending_bytes == 0) return;
    uint64_t interval = now - g_dbglog_rx_last_ms;
    double mbps = g_dbglog_rx_active_us
                ? (double)g_dbglog_rx_pending_bytes / (double)g_dbglog_rx_active_us
                : 0.0;
    char line[384];
    snprintf(line, sizeof(line),
             "%llu\tWS_RX\t-\t-\tbytes=%llu\tcumulative=%llu\twall_ms=%llu\tactive_us=%llu\tMB/s=%.2f\tlast_segment=%llu\n",
             (unsigned long long)now,
             (unsigned long long)g_dbglog_rx_pending_bytes,
             (unsigned long long)g_dbglog_rx_bytes,
             (unsigned long long)interval,
             (unsigned long long)g_dbglog_rx_active_us, mbps,
             (unsigned long long)g_dbglog_rx_last_segment);
    dbglog_write(line);
    g_dbglog_rx_pending_bytes = 0;
    g_dbglog_rx_active_us = 0;
    g_dbglog_rx_last_ms = now;
}

static void dbglog_write_busy_sample(uint64_t now) {
    if (!g_dbglog_fp || g_dbglog_busy_pending_attempts == 0) return;
    uint64_t interval = now - g_dbglog_busy_last_ms;
    char line[384];
    snprintf(line, sizeof(line),
             "%llu\tWS_BACKPRESSURE\t-\t-\tattempts=%llu\tbytes=%llu\tcumulative_attempts=%llu\tinterval_ms=%llu\tlast_segment=%llu\n",
             (unsigned long long)now,
             (unsigned long long)g_dbglog_busy_pending_attempts,
             (unsigned long long)g_dbglog_busy_pending_bytes,
             (unsigned long long)g_dbglog_busy_attempts,
             (unsigned long long)interval,
             (unsigned long long)g_dbglog_busy_last_segment);
    dbglog_write(line);
    g_dbglog_busy_pending_attempts = 0;
    g_dbglog_busy_pending_bytes = 0;
    g_dbglog_busy_last_ms = now;
}

static void dbglog_write_cache_sample(uint64_t now) {
    if (!g_dbglog_fp || !g_cache_dirty) return;
    char line[384];
    snprintf(line, sizeof(line),
             "%llu\tWS_CACHE\t-\t-\t-\tduplicate_bytes=%llu\treloaded_bytes=%llu\tunread_evicted_bytes=%llu\n",
             (unsigned long long)now,
             (unsigned long long)g_cache_duplicate,
             (unsigned long long)g_cache_reload,
             (unsigned long long)g_cache_unread);
    dbglog_write(line);
    g_cache_last_ms = now;
    g_cache_dirty = 0;
}

void stream_debug_log_ws_cache(uint64_t duplicate_bytes, uint64_t reload_bytes,
                               uint64_t unread_evicted_bytes) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (g_dbglog_fp) {
        g_cache_duplicate += duplicate_bytes;
        g_cache_reload += reload_bytes;
        g_cache_unread += unread_evicted_bytes;
        g_cache_dirty = 1;
        uint64_t now = dbglog_elapsed_ms();
        if (now - g_cache_last_ms >= 1000) dbglog_write_cache_sample(now);
    }
    pthread_mutex_unlock(&g_dbglog_mutex);
}

static void dbglog_write_sender_sample(uint64_t now) {
    if (!g_dbglog_fp || !g_sender_dirty) return;
    char line[384];
    snprintf(line, sizeof(line),
             "%llu\tWS_SENDER\t-\t-\t-\tread_wait_us=%llu\tack_latency_us=%llu\tack_count=%llu\tsent_count=%llu\twindow=%llu\n",
             (unsigned long long)now,
             (unsigned long long)g_sender_read_us,
             (unsigned long long)g_sender_ack_us,
             (unsigned long long)g_sender_acks,
             (unsigned long long)g_sender_sent,
             (unsigned long long)g_sender_window);
    dbglog_write(line);
    g_sender_last_ms = now;
    g_sender_dirty = 0;
}

void stream_debug_log_ws_sender(uint64_t read_us, uint64_t ack_us,
                                uint64_t acks, uint64_t sent, uint64_t window) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (g_dbglog_fp) {
        g_sender_read_us = read_us;
        g_sender_ack_us = ack_us;
        g_sender_acks = acks;
        g_sender_sent = sent;
        g_sender_window = window;
        g_sender_dirty = 1;
        uint64_t now = dbglog_elapsed_ms();
        if (now - g_sender_last_ms >= 1000) dbglog_write_sender_sample(now);
    }
    pthread_mutex_unlock(&g_dbglog_mutex);
}

static const char *dbglog_get_dir(void) {
    const char *env = getenv("PKG_DEBUG_DIR");
    if (env && env[0] != '\0') return env;
    struct stat st;
    if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir("/data/pkgmgr", 0777);
        return "/data/pkgmgr";
    }
    return "/tmp";
}

int stream_debug_log_open(const char *title_id, const char *content_id,
                          const char *pkg_kind, const char *pkg_path,
                          uint64_t total_size) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (g_dbglog_fp) {
        fclose(g_dbglog_fp);
        g_dbglog_fp = NULL;
    }

    /* Include PID and sequence so same-second attempts never overwrite. */
    const char *tid = (title_id && title_id[0] != '\0') ? title_id : "UNKNOWN";
    const char *kind = (pkg_kind && pkg_kind[0] != '\0') ? pkg_kind : "unknown";
    const char *cid = (content_id && content_id[0] != '\0') ? content_id : "";

    time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    localtime_r(&now, &tmv);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);

    const char *dir = dbglog_get_dir();
    char filepath[1024];
    static unsigned session_sequence;
    snprintf(filepath, sizeof(filepath), "%s/stream_debug_%s_%s_%s_%d_%u.txt",
             dir, tid, kind, ts, (int)getpid(), ++session_sequence);

    g_dbglog_fp = fopen(filepath, "w");
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        install_log("[STREAM_DEBUG] Failed to create debug log: %s", filepath);
        return -1;
    }

    debug_log_retain_latest(dir, "stream_debug_", filepath, 20);

    g_dbglog_session_start_ms = dbglog_now_ms();
    g_cache_duplicate = g_cache_reload = g_cache_unread = 0;
    g_cache_last_ms = 0;
    g_cache_dirty = 0;
    g_sender_read_us = g_sender_ack_us = g_sender_acks = g_sender_sent = 0;
    g_sender_window = g_sender_last_ms = 0;
    g_sender_dirty = 0;
    g_dbglog_ws_bytes = 0;
    g_dbglog_ws_pending_bytes = 0;
    g_dbglog_ws_last_ms = 0;
    g_dbglog_ws_last_segment = 0;
    g_dbglog_rx_bytes = 0;
    g_dbglog_rx_pending_bytes = 0;
    g_dbglog_rx_active_us = 0;
    g_dbglog_rx_last_ms = 0;
    g_dbglog_rx_last_segment = 0;
    g_dbglog_busy_attempts = 0;
    g_dbglog_busy_pending_attempts = 0;
    g_dbglog_busy_pending_bytes = 0;
    g_dbglog_busy_last_ms = 0;
    g_dbglog_busy_last_segment = 0;

    /* Write file header with session metadata. */
    char hdr[3072];
    char date_str[64];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", &tmv);

    snprintf(hdr, sizeof(hdr),
             "# PKG Manager - Stream Debug Log\n"
             "# Generated: %s\n"
             "# build_version: %s\n"
             "# build_commit:  %s\n"
             "# build_date:    %s\n"
             "#\n"
             "# title_id:   %s\n"
             "# content_id: %s\n"
             "# pkg_kind:   %s\n"
             "# pkg_path:   %.400s\n"
             "# total_size: %llu\n"
             "#\n"
             "# Format: Each line is a tab-separated event record.\n"
             "# Fields: elapsed_ms \\t event_type \\t conn_id \\t req_no \\t peer \\t details...\n"
             "#\n"
             "# Event types:\n"
             "#   INSTALL_EVENT - Installer/helper lifecycle, IPC and native service diagnostics\n"
             "#   CONN_OPEN    - New TCP connection accepted\n"
             "#   REQUEST      - HTTP request received and response sent\n"
             "#   BODY_DONE    - Response body fully/partially delivered\n"
             "#   WS_RX       - WebSocket payload receive rate while bytes are arriving\n"
             "#   WS_ACCEPT   - Aggregated bytes accepted into the live RAM stream\n"
             "#   WS_BACKPRESSURE - Aggregated busy writes caused by the full RAM ring\n"
             "#   WS_CACHE    - Cumulative duplicate_bytes (already resident), reloaded_bytes (previously evicted), unread_evicted_bytes (no read during residency)\n"
             "#   WS_SENDER   - Browser cumulative read wait and ACK latency; ACK timings overlap with window=2 and include transfer time\n"
             "#   CONN_CLOSE   - TCP connection closed\n"
             "#\n\n",
             date_str, PKGMGR_VERSION, PKGMGR_BUILD_COMMIT, PKGMGR_BUILD_DATE,
             tid, cid, kind,
             pkg_path ? pkg_path : "",
             (unsigned long long)total_size);

    dbglog_write(hdr);
    pthread_mutex_unlock(&g_dbglog_mutex);

    install_log("[STREAM_DEBUG] Debug session opened: %s", filepath);
    return 0;
}

void stream_debug_log_conn_open(int conn_id, const char *peer) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    char line[512];
    snprintf(line, sizeof(line), "%llu\tCONN_OPEN\t%d\t-\t%s\n",
             (unsigned long long)dbglog_elapsed_ms(), conn_id,
             peer ? peer : "unknown");
    dbglog_write(line);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_request(int conn_id, int req_no, const char *peer,
                              const char *method, const char *path,
                              int has_range, uint64_t range_start, uint64_t range_end,
                              int http_status, uint64_t content_len,
                              uint64_t total_size) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    char line[2048];
    if (has_range) {
        snprintf(line, sizeof(line),
                 "%llu\tREQUEST\t%d\t%d\t%s\t%s\t%.400s\tRange: bytes=%llu-%llu\t%d\tContent-Length: %llu\tTotal: %llu\n",
                 (unsigned long long)dbglog_elapsed_ms(), conn_id, req_no,
                 peer ? peer : "unknown",
                 method ? method : "?",
                 path ? path : "?",
                 (unsigned long long)range_start, (unsigned long long)range_end,
                 http_status,
                 (unsigned long long)content_len,
                 (unsigned long long)total_size);
    } else {
        snprintf(line, sizeof(line),
                 "%llu\tREQUEST\t%d\t%d\t%s\t%s\t%.400s\tno-range\t%d\tContent-Length: %llu\tTotal: %llu\n",
                 (unsigned long long)dbglog_elapsed_ms(), conn_id, req_no,
                 peer ? peer : "unknown",
                 method ? method : "?",
                 path ? path : "?",
                 http_status,
                 (unsigned long long)content_len,
                 (unsigned long long)total_size);
    }
    dbglog_write(line);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_response_done(int conn_id, int req_no, const char *peer,
                                    uint64_t bytes_sent, uint64_t content_len,
                                    const char *end_reason) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    char line[512];
    snprintf(line, sizeof(line),
             "%llu\tBODY_DONE\t%d\t%d\t%s\tsent=%llu/%llu\treason=%s\n",
             (unsigned long long)dbglog_elapsed_ms(), conn_id, req_no,
             peer ? peer : "unknown",
             (unsigned long long)bytes_sent, (unsigned long long)content_len,
             end_reason ? end_reason : "unknown");
    dbglog_write(line);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_ws_receive(uint64_t segment, uint64_t bytes,
                                 uint64_t receive_us) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    uint64_t now = dbglog_elapsed_ms();
    g_dbglog_rx_bytes += bytes;
    g_dbglog_rx_pending_bytes += bytes;
    g_dbglog_rx_active_us += receive_us;
    g_dbglog_rx_last_segment = segment;
    if (now - g_dbglog_rx_last_ms >= 1000) dbglog_write_rx_sample(now);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_ws_accept(uint64_t segment, uint64_t bytes) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    uint64_t now = dbglog_elapsed_ms();
    g_dbglog_ws_bytes += bytes;
    g_dbglog_ws_pending_bytes += bytes;
    g_dbglog_ws_last_segment = segment;
    if (now - g_dbglog_ws_last_ms >= 1000) {
        dbglog_write_ws_sample(now);
    }
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_ws_busy(uint64_t segment, uint64_t bytes) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    uint64_t now = dbglog_elapsed_ms();
    g_dbglog_busy_attempts++;
    g_dbglog_busy_pending_attempts++;
    g_dbglog_busy_pending_bytes += bytes;
    g_dbglog_busy_last_segment = segment;
    if (now - g_dbglog_busy_last_ms >= 1000) dbglog_write_busy_sample(now);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_conn_close(int conn_id, const char *peer, int reqs_served) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        return;
    }
    char line[512];
    snprintf(line, sizeof(line),
             "%llu\tCONN_CLOSE\t%d\t-\t%s\treqs_served=%d\n",
             (unsigned long long)dbglog_elapsed_ms(), conn_id,
             peer ? peer : "unknown",
             reqs_served);
    dbglog_write(line);
    pthread_mutex_unlock(&g_dbglog_mutex);
}

void stream_debug_log_close(void) {
    pthread_mutex_lock(&g_dbglog_mutex);
    if (g_dbglog_fp) {
        uint64_t now = dbglog_elapsed_ms();
        dbglog_write_rx_sample(now);
        dbglog_write_ws_sample(now);
        dbglog_write_busy_sample(now);
        dbglog_write_cache_sample(now);
        dbglog_write_sender_sample(now);
        char line[256];
        snprintf(line, sizeof(line), "\n# Session ended at elapsed_ms=%llu\n",
                 (unsigned long long)dbglog_elapsed_ms());
        dbglog_write(line);
        fclose(g_dbglog_fp);
        g_dbglog_fp = NULL;
        g_dbglog_session_start_ms = 0;
        g_dbglog_ws_bytes = 0;
        g_dbglog_ws_pending_bytes = 0;
        g_dbglog_ws_last_ms = 0;
        g_dbglog_ws_last_segment = 0;
        g_dbglog_rx_bytes = 0;
        g_dbglog_rx_pending_bytes = 0;
        g_dbglog_rx_active_us = 0;
        g_dbglog_rx_last_ms = 0;
        g_dbglog_rx_last_segment = 0;
        g_dbglog_busy_attempts = 0;
        g_dbglog_busy_pending_attempts = 0;
        g_dbglog_busy_pending_bytes = 0;
        g_dbglog_busy_last_ms = 0;
        g_dbglog_busy_last_segment = 0;
    }
    pthread_mutex_unlock(&g_dbglog_mutex);
    install_log("[STREAM_DEBUG] Debug session closed");
}

int stream_debug_log_is_active(void) {
    pthread_mutex_lock(&g_dbglog_mutex);
    int active = (g_dbglog_fp != NULL) ? 1 : 0;
    pthread_mutex_unlock(&g_dbglog_mutex);
    return active;
}
