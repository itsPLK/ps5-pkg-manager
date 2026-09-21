/*
 * PKG Manager - SMB Streaming Debug Logger
 *
 * See include/smb_debug_log.h for the public API.
 */

#include "smb_debug_log.h"
#include "installer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t g_smb_dbg_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE           *g_smb_dbg_fp    = NULL;
static int             g_smb_dbg_refcount = 0;
static uint64_t        g_smb_ses_start_ms = 0;
static uint64_t        g_smb_total_bytes  = 0; /* bytes delivered so far */
static uint64_t        g_smb_file_size    = 0;

uint64_t smb_dbg_now_us_internal(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000ULL +
               (uint64_t)ts.tv_nsec / 1000ULL;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint64_t smb_dbg_elapsed_ms(void) {
    return (smb_dbg_now_us_internal() / 1000ULL) - g_smb_ses_start_ms;
}

static void smb_dbg_write(const char *line) {
    /* Caller must hold g_smb_dbg_mutex */
    if (!g_smb_dbg_fp) return;
    fputs(line, g_smb_dbg_fp);
    fflush(g_smb_dbg_fp);
}

static const char *smb_dbg_get_dir(void) {
    const char *env = getenv("PKG_DEBUG_DIR");
    if (env && env[0] != '\0') return env;
    struct stat st;
    if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir("/data/pkgmgr", 0777);
        return "/data/pkgmgr";
    }
    return "/tmp";
}

int smb_debug_log_open(const char *server, const char *share,
                       const char *smb_url, uint64_t file_size) {
    pthread_mutex_lock(&g_smb_dbg_mutex);
    if (g_smb_dbg_fp) {
        g_smb_dbg_refcount++;
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        return 0;
    }
    g_smb_total_bytes = 0;
    g_smb_file_size   = file_size;

    time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    localtime_r(&now, &tmv);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);

    /* Sanitise server name for use in a filename (replace / : etc.) */
    char srv_safe[64];
    strncpy(srv_safe, server ? server : "unknown", sizeof(srv_safe) - 1);
    srv_safe[sizeof(srv_safe) - 1] = '\0';
    for (char *p = srv_safe; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == ' ') *p = '_';
    }

    const char *dir = smb_dbg_get_dir();
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/smb_debug_%s_%s.txt",
             dir, srv_safe, ts);

    g_smb_dbg_fp = fopen(filepath, "w");
    if (!g_smb_dbg_fp) {
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        install_log("[SMB_DEBUG] Failed to create debug log: %s", filepath);
        return -1;
    }

    g_smb_dbg_refcount = 1;
    g_smb_ses_start_ms = smb_dbg_now_us_internal() / 1000ULL;

    char date_str[64];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", &tmv);

    char hdr[2048];
    snprintf(hdr, sizeof(hdr),
             "# PKG Manager - SMB Streaming Debug Log\n"
             "# Generated:  %s\n"
             "#\n"
             "# server:     %s\n"
             "# share:      %s\n"
             "# smb_url:    %.400s\n"
             "# file_size:  %llu bytes\n"
             "#\n"
             "# Format: tab-separated event records.\n"
             "#\n"
             "# READ events (one per smb_file_session_read call):\n"
             "#   elapsed_ms TAB READ TAB offset TAB requested TAB returned TAB elapsed_us TAB throughput_MBps TAB n_slots TAB cumulative_MBps\n"
             "#\n"
             "# SLOT events (one per async pipeline slot completion):\n"
             "#   elapsed_ms TAB SLOT TAB slot_idx TAB offset TAB requested TAB returned TAB rtt_us TAB slot_MBps\n"
             "#\n\n",
             date_str,
             server ? server : "?",
             share  ? share  : "?",
             smb_url ? smb_url : "?",
             (unsigned long long)file_size);

    smb_dbg_write(hdr);
    pthread_mutex_unlock(&g_smb_dbg_mutex);

    install_log("[SMB_DEBUG] Debug log opened: %s", filepath);
    return 0;
}

void smb_debug_log_read(uint64_t offset, size_t requested,
                        ssize_t returned, uint64_t elapsed_us,
                        int n_slots) {
    pthread_mutex_lock(&g_smb_dbg_mutex);
    if (!g_smb_dbg_fp) {
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        return;
    }

    if (returned > 0)
        g_smb_total_bytes += (uint64_t)returned;

    /* Call throughput: MBps = (bytes / elapsed_us) × 1e6 / 1e6 = bytes/us */
    double call_mbps = 0.0;
    if (elapsed_us > 0 && returned > 0)
        call_mbps = (double)returned / (double)elapsed_us; /* MB/s (bytes/µs == MB/s) */

    /* Cumulative average throughput since session open */
    double cum_mbps = 0.0;
    uint64_t elapsed_total_us = smb_dbg_elapsed_ms() * 1000ULL;
    if (elapsed_total_us > 0 && g_smb_total_bytes > 0)
        cum_mbps = (double)g_smb_total_bytes / (double)elapsed_total_us;

    char line[512];
    snprintf(line, sizeof(line),
             "%llu\tREAD\t%llu\t%zu\t%zd\t%llu\t%.2f\t%d\t%.2f\n",
             (unsigned long long)smb_dbg_elapsed_ms(),
             (unsigned long long)offset,
             requested,
             returned,
             (unsigned long long)elapsed_us,
             call_mbps,
             n_slots,
             cum_mbps);
    smb_dbg_write(line);
    pthread_mutex_unlock(&g_smb_dbg_mutex);
}

void smb_debug_log_slot(int slot_idx, uint64_t offset,
                        uint32_t requested, int returned,
                        uint64_t rtt_us) {
    pthread_mutex_lock(&g_smb_dbg_mutex);
    if (!g_smb_dbg_fp) {
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        return;
    }

    double slot_mbps = 0.0;
    if (rtt_us > 0 && returned > 0)
        slot_mbps = (double)returned / (double)rtt_us;

    char line[512];
    snprintf(line, sizeof(line),
             "%llu\tSLOT\t%d\t%llu\t%u\t%d\t%llu\t%.2f\n",
             (unsigned long long)smb_dbg_elapsed_ms(),
             slot_idx,
             (unsigned long long)offset,
             requested,
             returned,
             (unsigned long long)rtt_us,
             slot_mbps);
    smb_dbg_write(line);
    pthread_mutex_unlock(&g_smb_dbg_mutex);
}

void smb_debug_log_close(void) {
    pthread_mutex_lock(&g_smb_dbg_mutex);
    if (!g_smb_dbg_fp) {
        if (g_smb_dbg_refcount > 0) g_smb_dbg_refcount--;
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        install_log("[SMB_DEBUG] Debug session closed (was not open)");
        return;
    }
    if (g_smb_dbg_refcount > 0) {
        g_smb_dbg_refcount--;
    }
    if (g_smb_dbg_refcount > 0) {
        pthread_mutex_unlock(&g_smb_dbg_mutex);
        return;
    }

    uint64_t elapsed_ms   = smb_dbg_elapsed_ms();
    double   total_mbps   = 0.0;
    if (elapsed_ms > 0)
        total_mbps = (double)g_smb_total_bytes / ((double)elapsed_ms * 1000.0);

    char footer[512];
    snprintf(footer, sizeof(footer),
             "\n"
             "# Session summary\n"
             "# total_delivered: %llu bytes (%.1f MB)\n"
             "# file_size:       %llu bytes\n"
             "# wall_time:       %llu ms\n"
             "# average_speed:   %.2f MB/s\n",
             (unsigned long long)g_smb_total_bytes,
             (double)g_smb_total_bytes / (1024.0 * 1024.0),
             (unsigned long long)g_smb_file_size,
             (unsigned long long)elapsed_ms,
             total_mbps);
    smb_dbg_write(footer);

    fclose(g_smb_dbg_fp);
    g_smb_dbg_fp       = NULL;
    g_smb_total_bytes  = 0;
    g_smb_file_size    = 0;
    g_smb_ses_start_ms = 0;
    pthread_mutex_unlock(&g_smb_dbg_mutex);
    install_log("[SMB_DEBUG] Debug session closed");
}
