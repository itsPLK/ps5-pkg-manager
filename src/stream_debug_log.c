/*
 * PKG Manager - Stream Debug File Logger
 *
 * Records every TCP connection and HTTP range request from the PS5 system
 * client to the stream server, producing a text log that can be used to
 * build an exact mock / replay of the download pattern for remote
 * (WebSocket) streaming development.
 */

#include "stream_debug_log.h"
#include "installer.h"

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

    /* Build filename: stream_debug_<title_id>_<kind>_<YYYYMMDD_HHMMSS>.txt */
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
    snprintf(filepath, sizeof(filepath), "%s/stream_debug_%s_%s_%s.txt", dir, tid, kind, ts);

    g_dbglog_fp = fopen(filepath, "w");
    if (!g_dbglog_fp) {
        pthread_mutex_unlock(&g_dbglog_mutex);
        install_log("[STREAM_DEBUG] Failed to create debug log: %s", filepath);
        return -1;
    }

    g_dbglog_session_start_ms = dbglog_now_ms();

    /* Write file header with session metadata. */
    char hdr[2048];
    char date_str[64];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", &tmv);

    snprintf(hdr, sizeof(hdr),
             "# PKG Manager - Stream Debug Log\n"
             "# Generated: %s\n"
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
             "#   CONN_OPEN    - New TCP connection accepted\n"
             "#   REQUEST      - HTTP request received and response sent\n"
             "#   BODY_DONE    - Response body fully/partially delivered\n"
             "#   CONN_CLOSE   - TCP connection closed\n"
             "#\n\n",
             date_str, tid, cid, kind,
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
        char line[256];
        snprintf(line, sizeof(line), "\n# Session ended at elapsed_ms=%llu\n",
                 (unsigned long long)dbglog_elapsed_ms());
        dbglog_write(line);
        fclose(g_dbglog_fp);
        g_dbglog_fp = NULL;
        g_dbglog_session_start_ms = 0;
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
