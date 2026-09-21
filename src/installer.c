/*
 * PKG Manager - Package Installation Engine
 *
 * Coordinates package streaming, multi-part verification,
 * AppInstUtil integration, and background installation progress.
 */

#include "installer.h"
#include "pkg_parser.h"
#include "pkg_scanner.h"
#include "multipart.h"
#include "notification.h"
#include "app_info.h"
#include "stream_server.h"
#include "stream_debug_log.h"
#include "pkg_cache.h"
#include "ws_stream.h" /* NEW: live RAM sessions (additive; worker below unchanged) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>

#if defined(__Prospero__) || defined(PS5_BUILD)
typedef struct pkg_metadata {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
} pkg_metadata_t;

typedef struct pkg_info {
    char content_id[48];
    int type;
    int platform;
} pkg_info_t;

typedef struct playgo_info {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][48];
    unsigned char unknown[6480];
} playgo_info_t;

typedef struct {
    int32_t error_code;
    int32_t version;
    char description[512];
    char type[9];
} SceAppInstallErrorInfo;

typedef struct {
    char status[16];
    char src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size;
    uint64_t initial_chunk_size;
    uint64_t total_size;
    uint32_t promote_progress;
    SceAppInstallErrorInfo error_info;
    int32_t local_copy_percent;
    bool is_copy_only;
} SceAppInstallStatusInstalled;

extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);
extern int sceAppInstUtilInstallByPackage(const pkg_metadata_t *meta, pkg_info_t *info, playgo_info_t *playgo);
extern int sceAppInstUtilGetInstallStatus(const char* content_id, SceAppInstallStatusInstalled* status);
#endif

static installer_status_t g_status;
static pthread_mutex_t g_installer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_monitor_thread;
static volatile int g_monitor_running = 0;
static volatile int g_monitor_thread_created = 0;
static pthread_t g_stream_thread;
static volatile int g_stream_thread_created = 0;
static volatile int g_cancel_stream = 0;

static int mkdir_recursive(const char *dir_path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                mkdir(tmp, 0777);
            }
            *p = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return mkdir(tmp, 0777);
    }
    return 0;
}

static uint64_t get_available_disk_space(const char *path) {
    if (getenv("PKG_FORCE_SPACE_CHECK_FAIL")) {
        return 1024; /* 1 KB to test space failure path */
    }

    struct statvfs sv;
    if (statvfs(path, &sv) == 0) {
        uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        return (uint64_t)sv.f_bavail * bsize;
    }

    /* If path does not exist yet, walk up to its nearest existing parent directory */
    char parent[512];
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *slash = strrchr(parent, '/');
    while (slash) {
        if (slash == parent) {
            parent[1] = '\0';
        } else {
            *slash = '\0';
        }
        if (statvfs(parent, &sv) == 0) {
            uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
            return (uint64_t)sv.f_bavail * bsize;
        }
        if (slash == parent) break;
        slash = strrchr(parent, '/');
    }

    if (statvfs("/data", &sv) == 0) {
        uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        return (uint64_t)sv.f_bavail * bsize;
    }
    if (statvfs("/tmp", &sv) == 0) {
        uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        return (uint64_t)sv.f_bavail * bsize;
    }
    return (uint64_t)-1;
}

static void *installer_monitor_worker(void *arg) {
    (void)arg;
    /* Intentionally passive: tests require that the browser is NOT
     * auto-reopened during an install (user may use an application while a big
     * install runs in the background). last_poll_time is still maintained
     * by installer_record_poll()/installer_status_to_json() for future
     * diagnostics, but no launch happens here. */
    while (g_monitor_running) {
        usleep(500000); /* 500ms */
    }
    return NULL;
}


static void cleanup_tmp_dir(const char *dir_path) {
    if (!dir_path || dir_path[0] == '\0') return;
    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
        unlink(file_path);
    }
    closedir(d);
}

/* Maximum in-memory log history: 2048 lines of up to 512 bytes
 * (temporarily raised from 256 so verbose stream-server diagnostics
 * survive a full install without rolling off /api/log). */
#define MAX_LOG_LINES 2048
#define MAX_LOG_LINE_LEN 512
#define MAX_LOG_FILE_SIZE (128 * 1024)
#define DEFAULT_LOG_FILE_PATH "/data/pkgmgr/install.log"

static char s_log_buffer[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
static int s_log_head = 0;
static int s_log_count = 0;
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_log_file_path[256] = DEFAULT_LOG_FILE_PATH;

void install_log_set_file_path(const char *path) {
    pthread_mutex_lock(&s_log_mutex);
    if (!path) {
        const char *env_log = getenv("PKG_LOG_FILE");
        if (env_log && env_log[0] != '\0') {
            if (strcmp(env_log, "none") == 0) {
                s_log_file_path[0] = '\0';
            } else {
                strncpy(s_log_file_path, env_log, sizeof(s_log_file_path) - 1);
                s_log_file_path[sizeof(s_log_file_path) - 1] = '\0';
            }
        } else if (getenv("PKG_NO_FILE_LOG") || getenv("PKG_DISABLE_FILE_LOG")) {
            s_log_file_path[0] = '\0';
        } else {
            strncpy(s_log_file_path, DEFAULT_LOG_FILE_PATH, sizeof(s_log_file_path) - 1);
            s_log_file_path[sizeof(s_log_file_path) - 1] = '\0';
        }
    } else if (path[0] == '\0' || strcmp(path, "none") == 0) {
        s_log_file_path[0] = '\0';
    } else {
        strncpy(s_log_file_path, path, sizeof(s_log_file_path) - 1);
        s_log_file_path[sizeof(s_log_file_path) - 1] = '\0';
    }
    pthread_mutex_unlock(&s_log_mutex);
}

void install_log_clear(void) {
    pthread_mutex_lock(&s_log_mutex);
    s_log_head = 0;
    s_log_count = 0;
    pthread_mutex_unlock(&s_log_mutex);
}

static int is_word_or_prefix(const char *msg, const char *target, int allow_prefix) {
    if (!msg || !target) return 0;
    size_t tlen = strlen(target);
    const char *p = msg;
    while ((p = strcasestr(p, target)) != NULL) {
        if (p == msg || !isalnum((unsigned char)*(p - 1))) {
            const char *end = p + tlen;
            if (allow_prefix) {
                return 1;
            }
            if (*end == '\0' || !isalnum((unsigned char)*end)) {
                return 1;
            }
        }
        p++;
    }
    return 0;
}

static int is_log_warning_or_error(const char *msg) {
    if (!msg) return 0;

    /* Check explicit bracketed/tagged prefixes */
    if (strcasestr(msg, "[error]") != NULL ||
        strcasestr(msg, "error:") != NULL ||
        strcasestr(msg, "[warn") != NULL ||
        strcasestr(msg, "warning:") != NULL ||
        strcasestr(msg, "warn:") != NULL ||
        strcasestr(msg, "[fail") != NULL) {
        return 1;
    }

    /* Word-bounded keywords to avoid false positives (e.g. Terror, Warner, 4160000) */
    if (is_word_or_prefix(msg, "error", 0) ||
        is_word_or_prefix(msg, "errors", 0) ||
        is_word_or_prefix(msg, "warn", 0) ||
        is_word_or_prefix(msg, "warned", 0) ||
        is_word_or_prefix(msg, "warning", 0) ||
        is_word_or_prefix(msg, "warnings", 0) ||
        is_word_or_prefix(msg, "fail", 1) ||      /* fail, failed, failure, failing */
        is_word_or_prefix(msg, "timed out", 0) ||
        is_word_or_prefix(msg, "timeout", 0) ||
        is_word_or_prefix(msg, "underflow", 0) ||
        is_word_or_prefix(msg, "wrong disc", 0) ||
        is_word_or_prefix(msg, "wrong part", 0) ||
        is_word_or_prefix(msg, "reject", 1)) {    /* reject, rejected, rejecting */
        return 1;
    }
    return 0;
}

static void append_to_log_file(const char *filepath, const char *line) {
    if (!filepath || filepath[0] == '\0' || strcmp(filepath, "none") == 0) return;

    pthread_mutex_lock(&s_file_mutex);

    static char s_last_ensured_dir[256] = {0};
    char dir[256];
    strncpy(dir, filepath, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (strcmp(dir, s_last_ensured_dir) != 0) {
            mkdir_recursive(dir);
            strncpy(s_last_ensured_dir, dir, sizeof(s_last_ensured_dir) - 1);
            s_last_ensured_dir[sizeof(s_last_ensured_dir) - 1] = '\0';
        }
    }

    /* Enforce size cap to prevent flash storage exhaustion */
    FILE *f = NULL;
    struct stat st;
    if (stat(filepath, &st) == 0 && st.st_size > MAX_LOG_FILE_SIZE) {
        f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "[LOG ROTATED: exceeded %d bytes]\n", MAX_LOG_FILE_SIZE);
        }
    } else {
        f = fopen(filepath, "a");
    }

    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }

    pthread_mutex_unlock(&s_file_mutex);
}

void install_log(const char *fmt, ...) {
    char buf[MAX_LOG_LINE_LEN - 40];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Strip trailing \r and \n if present (matching ps5-payload-manager) */
    size_t blen = strlen(buf);
    while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r')) {
        buf[blen - 1] = '\0';
        blen--;
    }
    if (blen == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_info_storage;
    struct tm *tm_info = localtime_r(&now, &tm_info_storage);
    char time_str[32] = {0};
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "%lu", (unsigned long)now);
    }

    char full_line[MAX_LOG_LINE_LEN];
    snprintf(full_line, sizeof(full_line), "[%s] %s", time_str, buf);

    /* 1. Print to stdout */
    printf("%s\n", full_line);

    /* 2. Store in circular in-memory buffer (matching ps5-payload-manager) */
    char path_copy[256];
    pthread_mutex_lock(&s_log_mutex);
    strncpy(s_log_buffer[s_log_head], full_line, sizeof(s_log_buffer[s_log_head]) - 1);
    s_log_buffer[s_log_head][sizeof(s_log_buffer[s_log_head]) - 1] = '\0';
    s_log_head = (s_log_head + 1) % MAX_LOG_LINES;
    if (s_log_count < MAX_LOG_LINES) {
        s_log_count++;
    }
    strncpy(path_copy, s_log_file_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    pthread_mutex_unlock(&s_log_mutex);

    /* 3. Reduce logging to file: only persist errors and warnings if file path is active */
    if (path_copy[0] != '\0' && is_log_warning_or_error(buf)) {
        append_to_log_file(path_copy, full_line);
    }
}

char *install_log_get_text(size_t *out_len) {
    pthread_mutex_lock(&s_log_mutex);
    if (s_log_count == 0) {
        char path_copy[256];
        strncpy(path_copy, s_log_file_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';
        pthread_mutex_unlock(&s_log_mutex);

        /* In-memory buffer empty: fallback to disk file if configured and available */
        if (path_copy[0] != '\0' && strcmp(path_copy, "none") != 0) {
            pthread_mutex_lock(&s_file_mutex);
            FILE *f = fopen(path_copy, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                if (sz > 0) {
                    if (sz > 256 * 1024) sz = 256 * 1024;
                    fseek(f, -sz, SEEK_END);
                    char *disk_data = (char *)malloc(sz + 1);
                    if (disk_data) {
                        size_t rd = fread(disk_data, 1, sz, f);
                        disk_data[rd] = '\0';
                        fclose(f);
                        pthread_mutex_unlock(&s_file_mutex);
                        if (out_len) *out_len = rd;
                        return disk_data;
                    }
                }
                fclose(f);
            }
            pthread_mutex_unlock(&s_file_mutex);
        }

        char *fallback = strdup("No logs recorded yet.\n");
        if (out_len) *out_len = fallback ? strlen(fallback) : 0;
        return fallback;
    }

    size_t total_len = 0;
    for (int i = 0; i < s_log_count; i++) {
        int idx = (s_log_head - s_log_count + i + MAX_LOG_LINES) % MAX_LOG_LINES;
        total_len += strlen(s_log_buffer[idx]) + 1; /* +1 for newline */
    }

    char *buf = (char *)malloc(total_len + 1);
    if (!buf) {
        pthread_mutex_unlock(&s_log_mutex);
        char *fallback = strdup("Out of memory reading logs\n");
        if (out_len) *out_len = fallback ? strlen(fallback) : 0;
        return fallback;
    }

    size_t pos = 0;
    for (int i = 0; i < s_log_count; i++) {
        int idx = (s_log_head - s_log_count + i + MAX_LOG_LINES) % MAX_LOG_LINES;
        size_t line_len = strlen(s_log_buffer[idx]);
        memcpy(buf + pos, s_log_buffer[idx], line_len);
        pos += line_len;
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    pthread_mutex_unlock(&s_log_mutex);

    if (out_len) *out_len = pos;
    return buf;
}

/* Formats the disc/part wait prompt with a countdown derived from the
 * tick counter (4 ticks/second). Tick-derived so standby time doesn't
 * consume the budget. Must be called sparingly (every few seconds). */
static void set_wait_prompt(int is_disc, uint32_t part_index, uint32_t total_parts,
                            uint32_t noticed_part, const char *pkg_filename,
                            int ticks_left) {
    if (ticks_left < 0) ticks_left = 0;
    int sec_left = ticks_left / 4;
    unsigned mm = (unsigned)(sec_left / 60);
    unsigned ss = (unsigned)(sec_left % 60);
    char msg[256];
    if (is_disc) {
        if (noticed_part != 0) {
            snprintf(msg, sizeof(msg), "Disc %u detected! Please insert Disc %u of %u (%u:%02u left)",
                     noticed_part, part_index, total_parts, mm, ss);
        } else {
            snprintf(msg, sizeof(msg), "Please insert Disc %u of %u (%u:%02u left)",
                     part_index, total_parts, mm, ss);
        }
    } else {
        if (noticed_part != 0) {
            snprintf(msg, sizeof(msg), "Part %u detected! Waiting for Part %u of %u (%u:%02u left)",
                     noticed_part, part_index, total_parts, mm, ss);
        } else if (pkg_filename && pkg_filename[0] != '\0') {
            snprintf(msg, sizeof(msg), "Waiting for Part %u of %u for %s (%u:%02u left)",
                     part_index, total_parts, pkg_filename, mm, ss);
        } else {
            snprintf(msg, sizeof(msg), "Waiting for Part %u of %u (%u:%02u left)",
                     part_index, total_parts, mm, ss);
        }
    }
    pthread_mutex_lock(&g_installer_mutex);
    strncpy(g_status.prompt_message, msg, sizeof(g_status.prompt_message) - 1);
    g_status.prompt_message[sizeof(g_status.prompt_message) - 1] = '\0';
    pthread_mutex_unlock(&g_installer_mutex);
}

static int installer_wait_for_part(const uint8_t *package_uuid, const char *pkg_filename,
                                   uint32_t part_index, uint32_t total_parts,
                                   char *out_path, size_t out_max) {
    /* Snapshot install state under lock: g_status may be mutated by
     * cancel/status threads while we block for up to 60 minutes. */
    char pkg_path_copy[512];
    pthread_mutex_lock(&g_installer_mutex);
    strncpy(pkg_path_copy, g_status.pkg_path, sizeof(pkg_path_copy) - 1);
    pkg_path_copy[sizeof(pkg_path_copy) - 1] = '\0';
    pthread_mutex_unlock(&g_installer_mutex);
    int is_disc = (strstr(pkg_path_copy, "/mnt/disc") != NULL ||
                   strstr(pkg_path_copy, "/disc") != NULL ||
                   strstr(pkg_path_copy, "_disc") != NULL);

    if (is_disc) {
        install_log("[DISC] Prompting: Please insert Disc %u of %u for %s",
                    part_index, total_parts, pkg_filename ? pkg_filename : "package");

        pthread_mutex_lock(&g_installer_mutex);
        g_status.waiting_for_disc = 1;
        g_status.current_part = part_index;
        strncpy(g_status.status_str, "waiting_disc", sizeof(g_status.status_str) - 1);
        pthread_mutex_unlock(&g_installer_mutex);

        ps5_notify("Please insert Disc %u of %u", part_index, total_parts);
    } else {
        install_log("[PART] Prompting: Waiting for Part %u of %u for %s",
                    part_index, total_parts, pkg_filename ? pkg_filename : "package");

        pthread_mutex_lock(&g_installer_mutex);
        g_status.waiting_for_disc = 1;
        g_status.current_part = part_index;
        strncpy(g_status.status_str, "waiting_disc", sizeof(g_status.status_str) - 1);
        pthread_mutex_unlock(&g_installer_mutex);

        ps5_notify("Waiting for Part %u of %u", part_index, total_parts);
    }

    int timeout_ticks = 0;
    const int max_timeout_ticks = 60 * 60 * 4; /* 60 mins at 250ms */
    uint32_t last_wrong_part = 0;
    /* Initial prompts (with full-budget countdown); refreshed below. */
    set_wait_prompt(is_disc, part_index, total_parts, 0, pkg_filename, max_timeout_ticks);

    for (;;) {
        int still_installing = 0;
        pthread_mutex_lock(&g_installer_mutex);
        still_installing = g_status.is_installing;
        pthread_mutex_unlock(&g_installer_mutex);
        if (!g_monitor_running || g_cancel_stream || !still_installing) break;
        uint32_t detected_part = 0;
        if (pkg_scanner_find_part_ex(package_uuid, pkg_filename, part_index, out_path, out_max, &detected_part) == 0) {
            install_log("[%s] %s %u detected at '%s'! Resuming stream...",
                        is_disc ? "DISC" : "PART", is_disc ? "Disc" : "Part", part_index, out_path);

            pthread_mutex_lock(&g_installer_mutex);
            g_status.waiting_for_disc = 0;
            g_status.current_part = part_index;
            strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
            if (is_disc) {
                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                         "Disc %u detected! Installing...", part_index);
            } else {
                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                         "Part %u detected! Installing...", part_index);
            }
            pthread_mutex_unlock(&g_installer_mutex);

            if (is_disc) {
                ps5_notify("Disc %u detected! Resuming install...", part_index);
            } else {
                ps5_notify("Part %u detected! Resuming install...", part_index);
            }
            return 0;
        }

        if (detected_part != 0 && detected_part != part_index && detected_part != last_wrong_part) {
            last_wrong_part = detected_part;
            install_log("[%s] Wrong %s detected (%s %u inserted, expected %s %u)",
                        is_disc ? "DISC" : "PART", is_disc ? "disc" : "part",
                        is_disc ? "Disc" : "Part", detected_part,
                        is_disc ? "Disc" : "Part", part_index);
            set_wait_prompt(is_disc, part_index, total_parts, detected_part,
                            pkg_filename, max_timeout_ticks - timeout_ticks);
            if (is_disc) {
                ps5_notify("Disc %u detected! Please insert Disc %u", detected_part, part_index);
            } else {
                ps5_notify("Part %u detected! Waiting for Part %u", detected_part, part_index);
            }
        }

        for (int t = 0; t < 5; t++) {
            int cont = 0;
            pthread_mutex_lock(&g_installer_mutex);
            cont = (g_monitor_running && !g_cancel_stream && g_status.is_installing);
            pthread_mutex_unlock(&g_installer_mutex);
            if (!cont) break;
            usleep(50000); /* 50ms */
        }
        timeout_ticks++;
        /* Refresh the countdown every 5s; heartbeat log every 5 min. */
        if (timeout_ticks % 20 == 0) {
            set_wait_prompt(is_disc, part_index, total_parts, last_wrong_part,
                            pkg_filename, max_timeout_ticks - timeout_ticks);
        }
        if (timeout_ticks % 1200 == 0) {
            int sec_left = (max_timeout_ticks - timeout_ticks) / 4;
            install_log("[%s] Still waiting for %s %u (%u:%02u left)",
                        is_disc ? "DISC" : "PART", is_disc ? "Disc" : "Part", part_index,
                        (unsigned)(sec_left / 60), (unsigned)(sec_left % 60));
        }
        if (timeout_ticks >= max_timeout_ticks) {
            install_log("[%s] Timed out waiting for %s %u",
                        is_disc ? "DISC" : "PART", is_disc ? "Disc" : "Part", part_index);
            pthread_mutex_lock(&g_installer_mutex);
            g_status.is_installing = 0;
            g_status.failed = 1;
            g_status.error_code = -22;
            strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
            if (is_disc) {
                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                         "Timed out waiting for Disc %u", part_index);
            } else {
                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                         "Timed out waiting for Part %u", part_index);
            }
            pthread_mutex_unlock(&g_installer_mutex);
            if (is_disc) {
                ps5_notify("Timed out waiting for Disc %u!", part_index);
            } else {
                ps5_notify("Timed out waiting for Part %u!", part_index);
            }
            return -22;
        }
    }

    return -1;
}

static void installer_on_part_changed(uint32_t current_part, uint32_t total_parts) {
    pthread_mutex_lock(&g_installer_mutex);
    if (g_status.is_installing && g_status.is_multipart) {
        g_status.current_part = current_part;
        int is_disc = (strstr(g_status.pkg_path, "/mnt/disc") != NULL ||
                       strstr(g_status.pkg_path, "/disc") != NULL ||
                       strstr(g_status.pkg_path, "_disc") != NULL);
        if (is_disc) {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Streaming Disc %u of %u...", current_part, total_parts);
        } else {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Streaming Part %u of %u...", current_part, total_parts);
        }
    }
    pthread_mutex_unlock(&g_installer_mutex);
}

void installer_notify_bytes_streamed(uint64_t bytes_read) {
    pthread_mutex_lock(&g_installer_mutex);
    if (g_status.is_installing) {
        g_status.downloaded_bytes += bytes_read;
        g_status.stream_served_bytes += bytes_read;
        if (g_status.total_bytes > 0) {
            if (g_status.downloaded_bytes > g_status.total_bytes) {
                g_status.downloaded_bytes = g_status.total_bytes;
            }
            g_status.progress_percent = ((float)g_status.downloaded_bytes / (float)g_status.total_bytes) * 100.0f;
            if (g_status.progress_percent > 100.0f) g_status.progress_percent = 100.0f;
        }
    }
    pthread_mutex_unlock(&g_installer_mutex);
}

#if defined(__Prospero__) || defined(PS5_BUILD)
/* Human-readable names for installer/playgo error codes (verified against
   etaHEN error_translator and on-console results). Unknown codes -> NULL. */
static const char *installer_strerror(int code) {
    if (code == 0) {
        return "OK";
    }
    switch ((uint32_t)code) {
    case 0x80A30001u: return "APP_INSTALLER_ERROR_UNKNOWN";
    case 0x80A30002u: return "APP_INSTALLER_ERROR_NOSPACE";
    case 0x80A30003u: return "APP_INSTALLER_ERROR_PARAM";
    case 0x80B21164u: return "PLAYGO_ERROR_CORE_INVALID_CONTENT_ID";
    case 0x80B21167u: return "PLAYGO_ERROR_CORE_CONTENT_ID_MISMATCH";
    case 0x80B2116Au: return "PLAYGO_ERROR_CORE_REQUIRE_FULLY_INSTALLED_APPLICATION";
    case 0x80B2116Eu: return "PLAYGO_ERROR_CORE_INVALID_VERSION";
    case 0x80B21170u: return "PLAYGO_ERROR_CORE_PATCH_INVALID_RANGE";
    case 0x80B2116Fu: return "PLAYGO_ERROR_CORE_INVALID_SLOT";
    case 0x80B2100Du: return "PLAYGO_ERROR_CORE_NOT_READY";
    case 0x80B2100Eu: return "PLAYGO_ERROR_CORE_TIMEOUT";
    default: return NULL;
    }
}

/* Slot-family errors are transient (e.g. patch installed while the system
   still finalizes the base): safe to retry with a fresh session. Anything
   else, including PARAM, fails immediately. */
static int is_transient_slot_error(int code) {
    uint32_t c = (uint32_t)code;
    return c == 0x80B2116Fu || c == 0x80B2100Du || c == 0x80B2100Eu;
}
#endif

static void *stream_installer_worker(void *arg) {
    (void)arg;

    /* Snapshot state under lock at thread entry (happens-after
     * installer_start commit via pthread_create). */
    char worker_pkg_path[512];
    int worker_is_multipart = 0;
    pthread_mutex_lock(&g_installer_mutex);
    strncpy(worker_pkg_path, g_status.pkg_path, sizeof(worker_pkg_path) - 1);
    worker_pkg_path[sizeof(worker_pkg_path) - 1] = '\0';
    worker_is_multipart = g_status.is_multipart;
    pthread_mutex_unlock(&g_installer_mutex);

    multipart_header_t hdr1;
    memset(&hdr1, 0, sizeof(hdr1));
    uint32_t total_parts = 1;
    const char *orig_name = "package.pkg";

    if (worker_is_multipart) {
        if (multipart_read_header(worker_pkg_path, &hdr1) != 0) {
            pthread_mutex_lock(&g_installer_mutex);
            g_status.is_installing = 0;
            g_status.failed = 1;
            g_status.error_code = -20;
            strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Failed to read header of Part 1");
            pthread_mutex_unlock(&g_installer_mutex);
            install_log("[INSTALLER] Failed to read header of Part 1: %s", worker_pkg_path);
            return NULL;
        }

        total_parts = hdr1.total_parts;
        if (total_parts == 0 || total_parts > MAX_MULTIPART_PARTS) {
            pthread_mutex_lock(&g_installer_mutex);
            g_status.is_installing = 0;
            g_status.failed = 1;
            g_status.error_code = -25;
            strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Invalid total parts count (%u)", total_parts);
            pthread_mutex_unlock(&g_installer_mutex);
            return NULL;
        }

        orig_name = hdr1.pkg_filename[0] ? hdr1.pkg_filename : "package.pkg";
        const char *safe_slash = strrchr(orig_name, '/');
        if (safe_slash) orig_name = safe_slash + 1;
        if (orig_name[0] == '\0') orig_name = "package.pkg";
    } else {
        /* Single package: stream the file directly. Size and identity come
           from the parser via g_status; only multipart overrides them below. */
        const char *slash = strrchr(worker_pkg_path, '/');
        if (slash && slash[1] != '\0') {
            orig_name = slash + 1;
        }
    }

    char clean_pkg_name[256];
    strncpy(clean_pkg_name, orig_name, sizeof(clean_pkg_name) - 1);
    clean_pkg_name[sizeof(clean_pkg_name) - 1] = '\0';
    char *pdot = strstr(clean_pkg_name, ".part");
    if (pdot) {
        *pdot = '\0';
    }
    if (!strstr(clean_pkg_name, ".pkg") && !strstr(clean_pkg_name, ".PKG")) {
        strncat(clean_pkg_name, ".pkg", sizeof(clean_pkg_name) - strlen(clean_pkg_name) - 1);
    }

    /* Display name for the system installer UI. Static storage: ShellCore
       may read meta strings after return. STREAM_NAME_OVERRIDE (build-time
       -D) pins one literal for A/B runs; otherwise title ID + kind +
       version, which never contains the package title. */
    static char disp_name[320];
#ifdef STREAM_NAME_OVERRIDE
    strncpy(disp_name, STREAM_NAME_OVERRIDE, sizeof(disp_name) - 1);
    disp_name[sizeof(disp_name) - 1] = '\0';
#else
    /* Kind word only: no version, no title text. Version-bearing and
       title-bearing names have been rejected with 0x80A30003 while
       kind-only forms install cleanly. Snapshot IDs under lock. */
    {
        char tid_copy[32] = "PKG";
        char kind_copy[16] = "base";
        pthread_mutex_lock(&g_installer_mutex);
        if (g_status.title_id[0] != '\0') {
            strncpy(tid_copy, g_status.title_id, sizeof(tid_copy) - 1);
            tid_copy[sizeof(tid_copy) - 1] = '\0';
        }
        if (worker_is_multipart) {
            strncpy(kind_copy, hdr1.pkg_type, sizeof(kind_copy) - 1);
            kind_copy[sizeof(kind_copy) - 1] = '\0';
        } else if (g_status.pkg_kind[0] != '\0') {
            strncpy(kind_copy, g_status.pkg_kind, sizeof(kind_copy) - 1);
            kind_copy[sizeof(kind_copy) - 1] = '\0';
        }
        pthread_mutex_unlock(&g_installer_mutex);
        if (strcasecmp(kind_copy, "update") == 0) {
            snprintf(disp_name, sizeof(disp_name), "%s (Update)", tid_copy);
        } else if (strcasecmp(kind_copy, "dlc") == 0) {
            snprintf(disp_name, sizeof(disp_name), "%s (DLC)", tid_copy);
        } else {
            snprintf(disp_name, sizeof(disp_name), "%s (Base)", tid_copy);
        }
    }
#endif

    /* Start the raw-socket stream server for the system installer. */
    if (stream_server_session_start(worker_pkg_path) != 0) {
        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_installing = 0;
        g_status.failed = 1;
        g_status.error_code = -23;
        strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Failed to start virtual stream session");
        pthread_mutex_unlock(&g_installer_mutex);
        ps5_notify("Failed to start virtual stream!");
        install_log("[INSTALLER] Failed to start stream session for %s", worker_pkg_path);
        return NULL;
    }

    /* Activate stream debug file logging if the setting is enabled.
     * Opens after the stream server is up so total_size is finalized. */
    {
        app_settings_t dbg_settings;
        pkg_cache_get_settings(&dbg_settings);
        if (dbg_settings.pkg_install_debug) {
            char dbg_tid[32] = {0}, dbg_cid[64] = {0}, dbg_kind[16] = {0};
            uint64_t dbg_total = 0;
            pthread_mutex_lock(&g_installer_mutex);
            strncpy(dbg_tid, g_status.title_id, sizeof(dbg_tid) - 1);
            strncpy(dbg_cid, g_status.content_id, sizeof(dbg_cid) - 1);
            strncpy(dbg_kind, g_status.pkg_kind, sizeof(dbg_kind) - 1);
            dbg_total = g_status.total_bytes;
            pthread_mutex_unlock(&g_installer_mutex);
            stream_debug_log_open(dbg_tid, dbg_cid, dbg_kind, worker_pkg_path, dbg_total);
            /* Also enable verbose stream_server console logging when debug is active */
            stream_server_set_debug(1);
        }
    }

    /* Unique URI per install: the system remembers recently used stream URLs
       across payload restarts (reusing package-1.pkg after a redeploy gets
       rejected), so key by wall-clock timestamp plus a per-install sequence
       for same-second safety. The pinned basename is published to the raw
       server, which serves only that exact name (anything else 404s). */
    static unsigned int s_stream_seq = 0;
    char stream_uri[1024];
    snprintf(stream_uri, sizeof(stream_uri), "http://127.0.0.1:%d/stream/install/package-%lu-%u.pkg",
             STREAM_SERVER_PORT, (unsigned long)time(NULL), (unsigned int)++s_stream_seq);
    /* Pin the exact session filename so the stream server 404s any
     * foreign name (e.g. client-derived <content_id>.crc sidecars). */
    {
        const char *slash = strrchr(stream_uri, '/');
        stream_server_set_session_name(slash ? slash + 1 : stream_uri);
    }

    pthread_mutex_lock(&g_installer_mutex);
    g_status.waiting_for_disc = 0;
    strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
             "Installing package %.200s...", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
    g_status.downloaded_bytes = 0;
    g_status.stream_served_bytes = 0;
    if (g_status.is_multipart) {
        g_status.total_bytes = hdr1.total_pkg_size;
    }
    g_status.progress_percent = 0.0f;
    pthread_mutex_unlock(&g_installer_mutex);

    install_log("[INSTALLER] Initiating multi-part stream install: URI='%s', name='%s', total_bytes=%llu, parts=%u",
                stream_uri, disp_name, (unsigned long long)hdr1.total_pkg_size, total_parts);

#if defined(__Prospero__) || defined(PS5_BUILD)
    pkg_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.uri = stream_uri;
    meta.ex_uri = "";
    meta.playgo_scenario_id = "";
    meta.content_id = "";
    meta.content_name = disp_name;
    meta.icon_url = "";

    pkg_info_t info;
    playgo_info_t playgo;

    /* Slot-family errors are transient: retry with a fresh session + URI
       (initial try, then after 2s, then after 5s). Anything else, including
       PARAM, fails immediately. Waits abort promptly on cancel/shutdown. */
    static const int retry_delays[] = { 0, 2, 5 };
    int ret = -1;
    const char *rname = NULL;
    for (int attempt = 0; attempt < 3 && !g_cancel_stream; attempt++) {
        if (attempt > 0) {
            int wait_s = retry_delays[attempt];
            install_log("[INSTALLER] Transient installer error 0x%08X, retrying in %ds (attempt %d/3)...",
                        ret, wait_s, attempt + 1);
            pthread_mutex_lock(&g_installer_mutex);
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Retrying install in %ds (attempt %d/3)...", wait_s, attempt + 1);
            pthread_mutex_unlock(&g_installer_mutex);
            for (int w = 0; w < wait_s && !g_cancel_stream && g_monitor_running; w++) {
                sleep(1);
            }
            if (g_cancel_stream || !g_monitor_running) {
                break;
            }
            stream_server_session_stop();
            if (stream_server_session_start(worker_pkg_path) != 0) {
                install_log("[INSTALLER] Stream server restart failed; keeping last error");
                break;
            }
            snprintf(stream_uri, sizeof(stream_uri), "http://127.0.0.1:%d/stream/install/package-%lu-%u.pkg",
                     STREAM_SERVER_PORT, (unsigned long)time(NULL), (unsigned int)++s_stream_seq);
            {
                const char *slash = strrchr(stream_uri, '/');
                stream_server_set_session_name(slash ? slash + 1 : stream_uri);
            }
            meta.uri = stream_uri;
            memset(&info, 0, sizeof(info));
            memset(&playgo, 0, sizeof(playgo));
            install_log("[INSTALLER] Retry stream install: URI='%s'", stream_uri);
        }
        ret = sceAppInstUtilInstallByPackage(&meta, &info, &playgo);
        rname = installer_strerror(ret);
        install_log("[INSTALLER] sceAppInstUtilInstallByPackage returned 0x%08X (%s), content_id='%s'",
                    ret, rname ? rname : "unknown", info.content_id);
        if (ret == 0 || !is_transient_slot_error(ret)) {
            break;
        }
    }
    rname = installer_strerror(ret);

    if (g_cancel_stream || !g_monitor_running) {
        /* Canceled or shutting down during retry waits: cancel/shutdown owns
           the status, just stop the server and exit. */
        ws_live_abort();
        stream_server_session_stop();
        ws_live_destroy();
        return NULL;
    }

    if (ret != 0) {
        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_installing = 0;
        g_status.failed = 1;
        g_status.error_code = ret;
        strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
        pthread_mutex_unlock(&g_installer_mutex);
        ps5_notify("Install error: 0x%08X (%s)", ret, rname ? rname : "unknown");
        ws_live_abort();
        stream_server_session_stop();
        ws_live_destroy();
        return NULL;
    }

    pthread_mutex_lock(&g_installer_mutex);
    if (info.content_id[0] != '\0') {
        strncpy(g_status.content_id, info.content_id, sizeof(g_status.content_id) - 1);
    }
    pthread_mutex_unlock(&g_installer_mutex);

    /* Monitor package stream delivery and console installation */
    time_t last_log_time = time(NULL);
    uint64_t last_log_bytes = 0;
    int stream_done = 0;
    time_t stream_done_time = 0;

    for (;;) {
        int keep_going = 0;
        pthread_mutex_lock(&g_installer_mutex);
        keep_going = (g_monitor_running && !g_cancel_stream && g_status.is_installing);
        uint64_t down = g_status.downloaded_bytes;
        uint64_t total = g_status.total_bytes;
        int waiting_disc = g_status.waiting_for_disc;
        char title_id_copy[32] = {0};
        strncpy(title_id_copy, g_status.title_id, sizeof(title_id_copy) - 1);
        char status_copy[32] = {0};
        strncpy(status_copy, g_status.status_str, sizeof(status_copy) - 1);
        char kind_copy[16] = {0};
        strncpy(kind_copy, g_status.pkg_kind, sizeof(kind_copy) - 1);
        char content_copy[64] = {0};
        strncpy(content_copy, g_status.content_id, sizeof(content_copy) - 1);
        char expect_ver[32] = {0};
        strncpy(expect_ver, g_status.pkg_version, sizeof(expect_ver) - 1);
        pthread_mutex_unlock(&g_installer_mutex);
        if (!keep_going) break;

        /* Check system installer status if content_id is available */
        if (info.content_id[0] != '\0') {
            SceAppInstallStatusInstalled sys_status;
            memset(&sys_status, 0, sizeof(sys_status));
            if (sceAppInstUtilGetInstallStatus(info.content_id, &sys_status) == 0) {
                if (sys_status.error_info.error_code != 0 || strcmp(sys_status.status, "error") == 0 || strcmp(sys_status.status, "none") == 0) {
                    const char *sname = installer_strerror(sys_status.error_info.error_code);
                    install_log("[INSTALLER] System installer reported error 0x%08X (%s) (status='%s')",
                                sys_status.error_info.error_code, sname ? sname : "unknown", sys_status.status);
                    pthread_mutex_lock(&g_installer_mutex);
                    g_status.is_installing = 0;
                    g_status.failed = 1;
                    g_status.error_code = sys_status.error_info.error_code ? sys_status.error_info.error_code : -1;
                    strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
                    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                             "System install error: 0x%08X", g_status.error_code);
                    pthread_mutex_unlock(&g_installer_mutex);
                    ps5_notify("System install error: 0x%08X", g_status.error_code);
                    break;
                }

                /* Only trust a system playable/completed once our stream has
                   delivered 100%: base and patch share one content_id, so an
                   early poll sees the installed base (stale state) and would
                   stop the server before the patch downloads anything. */
                if ((strcmp(sys_status.status, "playable") == 0 || strcmp(sys_status.status, "completed") == 0) &&
                    stream_done) {
                    install_log("[INSTALLER] System install completed successfully (status='%s')", sys_status.status);
                    pthread_mutex_lock(&g_installer_mutex);
                    g_status.downloaded_bytes = g_status.total_bytes;
                    g_status.progress_percent = 100.0f;
                    g_status.is_installing = 0;
                    g_status.completed = 1;
                    strncpy(g_status.status_str, "playable", sizeof(g_status.status_str) - 1);
                    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                             "%s is ready to play!", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
                    pthread_mutex_unlock(&g_installer_mutex);
                    ps5_notify("%s is ready to play!", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
                    break;
                }

                /* Adopt system progress only up to what our stream actually
                   sent: base and patch share one content_id, so an early poll
                   otherwise imports the base's full size and fakes 100%.
                   Our own total_bytes is authoritative; never adopt theirs. */
                if (sys_status.downloaded_size > down) {
                    pthread_mutex_lock(&g_installer_mutex);
                    uint64_t adopt = sys_status.downloaded_size;
                    if (adopt > g_status.stream_served_bytes) {
                        adopt = g_status.stream_served_bytes;
                    }
                    if (adopt > g_status.downloaded_bytes) {
                        g_status.downloaded_bytes = adopt;
                        if (g_status.total_bytes > 0) {
                            g_status.progress_percent = ((float)adopt / (float)g_status.total_bytes) * 100.0f;
                            if (g_status.progress_percent > 100.0f) g_status.progress_percent = 100.0f;
                        }
                    }
                    down = (adopt > down) ? adopt : down;
                    pthread_mutex_unlock(&g_installer_mutex);
                }
            }
        }

        /* Check if 100% of bytes have been delivered over the stream */
        if (!stream_done && total > 0 && down >= total) {
            stream_done = 1;
            stream_done_time = time(NULL);
            install_log("[INSTALLER] Stream transfer 100%% complete (%llu / %llu bytes). Waiting for system finalization...",
                        (unsigned long long)down, (unsigned long long)total);
            pthread_mutex_lock(&g_installer_mutex);
            strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Finishing installation of %s...", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
            pthread_mutex_unlock(&g_installer_mutex);
        }

        /* Once all stream bytes are sent, check if app has been registered installed.
         * Base presence alone is NOT enough for updates/DLC (base already exists):
         * gate those on DLC content_id or installed version >= PKG version. */
        if (stream_done) {
            if (time(NULL) - stream_done_time >= 3 && title_id_copy[0] != '\0') {
                int verified = 0;
                if ((strcasecmp(kind_copy, "dlc") == 0) && content_copy[0] != '\0') {
                    if (app_info_check_dlc_installed(title_id_copy, content_copy)) {
                        install_log("[INSTALLER] DLC verified installed: %s / %s", title_id_copy, content_copy);
                        verified = 1;
                    }
                } else if (expect_ver[0] != '\0') {
                    char installed_ver[32] = {0};
                    if (app_info_check_installed(title_id_copy, installed_ver, sizeof(installed_ver))) {
                        if (installed_ver[0] != '\0' &&
                            app_info_compare_versions(installed_ver, expect_ver) >= 0) {
                            install_log("[INSTALLER] %s verified installed: %s %s >= %s",
                                        (strcasecmp(kind_copy, "update") == 0) ? "Update" : "App",
                                        title_id_copy, installed_ver, expect_ver);
                            verified = 1;
                        } else if (installed_ver[0] != '\0') {
                            install_log("[INSTALLER] %s not yet applied: %s installed=%s expect=%s",
                                        (strcasecmp(kind_copy, "update") == 0) ? "Update" : "App",
                                        title_id_copy,
                                        installed_ver,
                                        expect_ver);
                        } else if (strcasecmp(kind_copy, "update") != 0) {
                            install_log("[INSTALLER] App verified installed in database: %s", title_id_copy);
                            verified = 1;
                        }
                    }
                } else {
                    if (app_info_check_installed(title_id_copy, NULL, 0)) {
                        install_log("[INSTALLER] App verified installed in database: %s", title_id_copy);
                        verified = 1;
                    }
                }
                if (verified) {
                    pthread_mutex_lock(&g_installer_mutex);
                    g_status.downloaded_bytes = g_status.total_bytes;
                    g_status.progress_percent = 100.0f;
                    g_status.is_installing = 0;
                    g_status.completed = 1;
                    strncpy(g_status.status_str, "playable", sizeof(g_status.status_str) - 1);
                    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                             "%s is ready to play!", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
                    pthread_mutex_unlock(&g_installer_mutex);
                    ps5_notify("%s is ready to play!", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
                    break;
                }
            }

            if (time(NULL) - stream_done_time > 300) {
                install_log("[INSTALLER] Finalization timed out after 5 minutes");
                pthread_mutex_lock(&g_installer_mutex);
                g_status.is_installing = 0;
                g_status.failed = 1;
                g_status.error_code = -24;
                strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                         "Installation timed out during finalization");
                pthread_mutex_unlock(&g_installer_mutex);
                ps5_notify("Installation timed out during finalization!");
                break;
            }
        }

        time_t now = time(NULL);
        if (now - last_log_time >= 5) {
            if (down != last_log_bytes || waiting_disc) {
                install_log("[STREAM] %llu / %llu bytes (%.1f%%), status='%s', waiting_disc=%d",
                            (unsigned long long)down, (unsigned long long)total,
                            total > 0 ? ((float)down / (float)total) * 100.0f : 0.0f,
                            status_copy, waiting_disc);
                last_log_bytes = down;
            }
            last_log_time = now;
        }

        sleep(1);
    }
#else
    /* Mock streaming simulation for host tests */
    char mock_pkg_path[512];
    pthread_mutex_lock(&g_installer_mutex);
    strncpy(mock_pkg_path, g_status.pkg_path, sizeof(mock_pkg_path) - 1);
    mock_pkg_path[sizeof(mock_pkg_path) - 1] = '\0';
    pthread_mutex_unlock(&g_installer_mutex);
    virtual_stream_t *vs_sim = (virtual_stream_t *)calloc(1, sizeof(virtual_stream_t));
    if (vs_sim && virtual_stream_open(mock_pkg_path, vs_sim) == 0) {
        char dummy[262144];
        uint64_t offset = 0;
        for (;;) {
            int run = 0;
            pthread_mutex_lock(&g_installer_mutex);
            run = (g_monitor_running && !g_cancel_stream && g_status.is_installing);
            pthread_mutex_unlock(&g_installer_mutex);
            if (!run || offset >= vs_sim->total_pkg_size) break;
            size_t rsize = sizeof(dummy);
            if (offset + rsize > vs_sim->total_pkg_size) {
                rsize = (size_t)(vs_sim->total_pkg_size - offset);
            }
            ssize_t n = virtual_stream_read(vs_sim, offset, dummy, rsize);
            if (n < 0) {
                break;
            }
            offset += (uint64_t)n;
            pthread_mutex_lock(&g_installer_mutex);
            g_status.downloaded_bytes = offset;
            if (vs_sim->total_pkg_size > 0) {
                g_status.progress_percent = ((float)offset / (float)vs_sim->total_pkg_size) * 100.0f;
                if (g_status.progress_percent > 100.0f) g_status.progress_percent = 100.0f;
            }
            pthread_mutex_unlock(&g_installer_mutex);
            usleep(1000);
        }
        virtual_stream_close(vs_sim);
    }
    if (vs_sim) free(vs_sim);

    pthread_mutex_lock(&g_installer_mutex);
    if (!g_cancel_stream && !g_status.failed && g_status.is_installing) {
        g_status.downloaded_bytes = g_status.total_bytes;
        g_status.progress_percent = 100.0f;
        strncpy(g_status.status_str, "playable", sizeof(g_status.status_str) - 1);
        g_status.is_installing = 0;
        g_status.completed = 1;
    }
    pthread_mutex_unlock(&g_installer_mutex);
#endif

    /* NEW: release the live RAM session (noop for disk installs). Abort
     * first so any reader blocked in ws_live_read wakes before/during
     * the stop's vs_refs drain; destroy frees the ring. */
    ws_live_abort();
    stream_server_session_stop();
    ws_live_destroy();
    return NULL;
}

int installer_init(const char *server_url) {
    pthread_mutex_lock(&g_installer_mutex);
    memset(&g_status, 0, sizeof(g_status));
    strncpy(g_status.status_str, "idle", sizeof(g_status.status_str) - 1);
    g_status.last_poll_time = time(NULL);
    g_cancel_stream = 0;

    (void)server_url;
    virtual_stream_set_part_finder(pkg_scanner_find_part);
    virtual_stream_set_disc_waiter(installer_wait_for_part);
    virtual_stream_set_part_notifier(installer_on_part_changed);

    const char *tmp_dir = getenv("PKG_TMP_DIR");
    if (!tmp_dir || tmp_dir[0] == '\0') {
        tmp_dir = PKG_DEFAULT_TMP_DIR;
    }
    cleanup_tmp_dir(tmp_dir);
    pthread_mutex_unlock(&g_installer_mutex);

#if defined(__Prospero__) || defined(PS5_BUILD)
    int ret = sceAppInstUtilInitialize();
    if (ret != 0) {
        printf("[PKG Manager] sceAppInstUtilInitialize returned 0x%08X\n", ret);
    }
#endif

    g_monitor_running = 1;
    if (pthread_create(&g_monitor_thread, NULL, installer_monitor_worker, NULL) != 0) {
        g_monitor_running = 0;
        g_monitor_thread_created = 0;
        return -1;
    }
    g_monitor_thread_created = 1;

    return 0;
}

int installer_start(const char *pkg_path) {
    if (!pkg_path || pkg_path[0] == '\0') {
        return -1;
    }

    /* Fast check under lock, then release before slow I/O (parse, statvfs,
     * mkdir) so cancel/status keep working. Re-checked under lock later. */
    pthread_mutex_lock(&g_installer_mutex);
    int already = g_status.is_installing;
    pthread_mutex_unlock(&g_installer_mutex);
    if (already) {
        return -2; /* Already installing */
    }

    /* Join any previous worker WITHOUT holding the mutex: the old worker
     * may be blocked trying to lock it (deadlock if we join while locked). */
    pthread_t old_thr;
    int have_old = 0;
    pthread_mutex_lock(&g_installer_mutex);
    if (g_stream_thread_created) {
        old_thr = g_stream_thread;
        have_old = 1;
        g_stream_thread_created = 0;
    }
    pthread_mutex_unlock(&g_installer_mutex);
    if (have_old) {
        pthread_join(old_thr, NULL);
    }

    char pkg_path_copy[512];
    strncpy(pkg_path_copy, pkg_path, sizeof(pkg_path_copy) - 1);
    pkg_path_copy[sizeof(pkg_path_copy) - 1] = '\0';

    pkg_detail_t detail;
    if (pkg_parser_parse(pkg_path_copy, &detail) != 0) {
        /* Fallback: populate basic info so installation can still proceed via sceAppInstUtil */
        memset(&detail, 0, sizeof(detail));
        strncpy(detail.path, pkg_path_copy, sizeof(detail.path) - 1);
        detail.path[sizeof(detail.path) - 1] = '\0';
        const char *slash = strrchr(pkg_path_copy, '/');
        if (slash) {
            strncpy(detail.filename, slash + 1, sizeof(detail.filename) - 1);
            detail.filename[sizeof(detail.filename) - 1] = '\0';
        } else {
            strncpy(detail.filename, pkg_path_copy, sizeof(detail.filename) - 1);
            detail.filename[sizeof(detail.filename) - 1] = '\0';
        }
        strncpy(detail.title_id, "UNKNOWN", sizeof(detail.title_id) - 1);
        strncpy(detail.title_name, "Package", sizeof(detail.title_name) - 1);
        /* smb:// paths have no local stat; leave sizes 0 (worker streams). */
        if (strncmp(pkg_path_copy, "smb://", 6) != 0) {
            struct stat st;
            if (stat(pkg_path_copy, &st) == 0) {
                detail.file_size = (uint64_t)st.st_size;
            }
        }
    }

    /* Multi-part packages are only supported on local drives (USB / optical discs) */
    if ((detail.is_multipart || strstr(pkg_path_copy, ".part") != NULL || strstr(pkg_path_copy, ".pkg.part") != NULL) &&
        strncmp(pkg_path_copy, "smb://", 6) == 0) {
        ps5_notify("Multi-part packages are only supported on USB/Disc!");
        return -13;
    }

    /* Validate storage space: requires only 1x storage (installed package size).
     * If an M.2 NVMe SSD (/mnt/ext1) is present, PS5 may be set to install to internal storage
     * or the M.2 drive. If neither drive has enough available space, block the installation.
     * If no M.2 SSD is present, validate against internal storage (/data). */
    uint64_t nvme_f = 0, nvme_t = 0, nvme_u = 0;
    int has_nvme = (system_get_nvme_storage_info(&nvme_f, &nvme_t, &nvme_u) == 0);
    uint64_t required_space = detail.total_pkg_size > 0 ? detail.total_pkg_size : detail.file_size;
    const char *check_dir = getenv("PKG_TMP_DIR");
    if (!check_dir || check_dir[0] == '\0') {
        check_dir = "/data";
    }

    uint64_t avail_space = get_available_disk_space(check_dir);
    int int_valid = (avail_space != (uint64_t)-1);
    uint64_t max_avail = int_valid ? avail_space : 0;
    if (has_nvme && nvme_f > max_avail) {
        max_avail = nvme_f;
    }

    if ((int_valid || has_nvme) && max_avail < required_space) {
        if (has_nvme) {
            ps5_notify("Not enough storage space! Need %llu MB (Internal: %llu MB, M.2: %llu MB)",
                       (unsigned long long)(required_space / (1024 * 1024)),
                       (unsigned long long)((int_valid ? avail_space : 0) / (1024 * 1024)),
                       (unsigned long long)(nvme_f / (1024 * 1024)));
        } else {
            ps5_notify("Not enough storage space! Need %llu MB, have %llu MB",
                       (unsigned long long)(required_space / (1024 * 1024)),
                       (unsigned long long)(avail_space / (1024 * 1024)));
        }
        return -10; /* Insufficient storage space */
    }

    /* Ensure staging directory exists for multi-part packages */
    if (detail.is_multipart) {
        const char *tmp_dir = getenv("PKG_TMP_DIR");
        if (!tmp_dir || tmp_dir[0] == '\0') {
            tmp_dir = PKG_DEFAULT_TMP_DIR;
        }
        if (mkdir_recursive(tmp_dir) != 0) {
            ps5_notify("Failed to create temporary directory %s", tmp_dir);
            return -11;
        }
        cleanup_tmp_dir(tmp_dir);
    }

    /* Commit under lock; re-check is_installing in case of a race. */
    pthread_mutex_lock(&g_installer_mutex);
    if (g_status.is_installing) {
        pthread_mutex_unlock(&g_installer_mutex);
        return -2;
    }
    memset(&g_status, 0, sizeof(g_status));
    g_status.is_installing = 1;
    g_status.is_multipart = detail.is_multipart;
    g_status.total_parts = detail.total_parts;
    g_status.current_part = detail.is_multipart ? 1 : 0;
    g_status.waiting_for_disc = 0;
    strncpy(g_status.pkg_path, pkg_path_copy, sizeof(g_status.pkg_path) - 1);
    g_status.pkg_path[sizeof(g_status.pkg_path) - 1] = '\0';
    strncpy(g_status.title_id, detail.title_id, sizeof(g_status.title_id) - 1);
    g_status.title_id[sizeof(g_status.title_id) - 1] = '\0';
    strncpy(g_status.title_name, detail.title_name, sizeof(g_status.title_name) - 1);
    g_status.title_name[sizeof(g_status.title_name) - 1] = '\0';
    strncpy(g_status.content_id, detail.content_id, sizeof(g_status.content_id) - 1);
    g_status.content_id[sizeof(g_status.content_id) - 1] = '\0';
    strncpy(g_status.pkg_kind, detail.pkg_type_str, sizeof(g_status.pkg_kind) - 1);
    g_status.pkg_kind[sizeof(g_status.pkg_kind) - 1] = '\0';
    strncpy(g_status.pkg_version, detail.app_version, sizeof(g_status.pkg_version) - 1);
    g_status.pkg_version[sizeof(g_status.pkg_version) - 1] = '\0';
    strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
    g_status.status_str[sizeof(g_status.status_str) - 1] = '\0';
    int is_disc_start = (strstr(pkg_path_copy, "/mnt/disc") != NULL ||
                         strstr(pkg_path_copy, "/disc") != NULL ||
                         strstr(pkg_path_copy, "_disc") != NULL);
    if (detail.is_multipart) {
        if (is_disc_start) {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Streaming Disc 1 of %u...", detail.total_parts);
        } else {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Streaming Part 1 of %u...", detail.total_parts);
        }
    } else {
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing %.200s...",
                 g_status.title_name[0] != '\0' ? g_status.title_name : "Package");
    }
    g_status.total_bytes = detail.total_pkg_size > 0 ? detail.total_pkg_size : detail.file_size;
    g_status.downloaded_bytes = 0;
    g_status.stream_served_bytes = 0;
    g_status.progress_percent = 0.0f;
    g_status.start_time = time(NULL);
    g_status.last_poll_time = time(NULL);
    g_cancel_stream = 0;

    /* Every install — single or multi-part — streams through the background
       worker so progress and completion are tracked uniformly.
       (Previous worker already joined above, without holding the lock.) */

    /* Launch background stream installer pipeline */
    if (pthread_create(&g_stream_thread, NULL, stream_installer_worker, NULL) != 0) {
        g_status.is_installing = 0;
        g_status.failed = 1;
        pthread_mutex_unlock(&g_installer_mutex);
        return -12;
    }
    g_stream_thread_created = 1;
    char notify_title[256];
    strncpy(notify_title, g_status.title_name[0] ? g_status.title_name : "Package",
            sizeof(notify_title) - 1);
    notify_title[sizeof(notify_title) - 1] = '\0';
    int notify_multipart = g_status.is_multipart;
    uint32_t notify_total = g_status.total_parts;
    pthread_mutex_unlock(&g_installer_mutex);
    if (notify_multipart) {
        if (is_disc_start) {
            ps5_notify("Installing %s (Disc 1 of %u)...", notify_title, notify_total);
        } else {
            ps5_notify("Installing %s (Part 1 of %u)...", notify_title, notify_total);
        }
    } else {
        ps5_notify("Installing %s...", notify_title);
    }
    return 0;
}

/* NEW: start an install from a live RAM session ("live:<id>", Direct
 * Install without any disk spool). Mirrors installer_start's checks and
 * commits, but metadata comes from pkg_parser_parse_mem over the uploaded
 * header cache and total size comes from the browser. Multi-part is
 * refused (live pushes are single packages). The existing background
 * worker runs unchanged: "live:<id>" flows through to
 * stream_server_session_start -> virtual_stream_open's live: scheme. */
int installer_start_live(const char *live_uri) {
    if (!live_uri || strncmp(live_uri, "live:", 5) != 0) {
        return -1;
    }
    const char *sid = live_uri + 5;
    if (sid[0] == '\0' || !ws_live_check_id(sid)) {
        return -1;
    }

    pthread_mutex_lock(&g_installer_mutex);
    int already = g_status.is_installing;
    pthread_mutex_unlock(&g_installer_mutex);
    if (already) {
        return -2;
    }

    pthread_t old_thr;
    int have_old = 0;
    pthread_mutex_lock(&g_installer_mutex);
    if (g_stream_thread_created) {
        old_thr = g_stream_thread;
        have_old = 1;
        g_stream_thread_created = 0;
    }
    pthread_mutex_unlock(&g_installer_mutex);
    if (have_old) {
        pthread_join(old_thr, NULL);
    }

    uint64_t live_total = ws_live_get_total();
    if (live_total == 0) {
        return -1;
    }

    /* Wait for the parse-ready header prefix (browser may still be
     * uploading it; the UI enables Install only at header_ready, so this
     * is normally immediate). */
    if (ws_live_wait_header(120) != 0) {
        ps5_notify("Live install: header timed out, re-upload the package");
        return -14;
    }

    uint8_t *hcache = (uint8_t *)malloc(WS_LIVE_SEG_SIZE);
    if (!hcache) {
        return -1;
    }
    size_t hlen = ws_live_get_header(hcache, WS_LIVE_SEG_SIZE);

    pkg_detail_t detail;
    int pm_stage = -1;
    if (hlen == 0 ||
        pkg_parser_parse_mem(hcache, hlen, live_total, live_uri, &detail,
                             &pm_stage) != 0) {
        /* Parity with disk installs: an unparseable header must not block
         * the install (the system reads content_id from the stream itself).
         * Log the stage so exotic layouts can be reported and fixed. */
        install_log("[INSTALLER] Live header parse failed (stage %d, %zu bytes); "
                    "proceeding with fallback metadata", pm_stage, hlen);
        memset(&detail, 0, sizeof(detail));
        strncpy(detail.path, live_uri, sizeof(detail.path) - 1);
        strncpy(detail.filename, "live-package.pkg", sizeof(detail.filename) - 1);
        strncpy(detail.title_id, "UNKNOWN", sizeof(detail.title_id) - 1);
        strncpy(detail.title_name, "Package", sizeof(detail.title_name) - 1);
        detail.total_pkg_size = live_total;
        detail.file_size = live_total;
    }
    free(hcache);

    if (detail.is_multipart) {
        ps5_notify("Live install supports single packages only");
        return -13;
    }

    char live_path_copy[512];
    strncpy(live_path_copy, live_uri, sizeof(live_path_copy) - 1);
    live_path_copy[sizeof(live_path_copy) - 1] = '\0';

    /* 1x space: installed output only, no spool copy. */
    uint64_t required_space = detail.total_pkg_size > 0 ? detail.total_pkg_size : live_total;
    uint64_t avail_space = get_available_disk_space("/data");
    if (avail_space != (uint64_t)-1 && avail_space < required_space) {
        ps5_notify("Not enough storage space! Need %llu MB, have %llu MB",
                   (unsigned long long)(required_space / (1024 * 1024)),
                   (unsigned long long)avail_space / (1024 * 1024));
        return -10;
    }

    pthread_mutex_lock(&g_installer_mutex);
    if (g_status.is_installing) {
        pthread_mutex_unlock(&g_installer_mutex);
        return -2;
    }
    memset(&g_status, 0, sizeof(g_status));
    g_status.is_installing = 1;
    g_status.is_multipart = 0;
    g_status.current_part = 0;
    strncpy(g_status.pkg_path, live_path_copy, sizeof(g_status.pkg_path) - 1);
    g_status.pkg_path[sizeof(g_status.pkg_path) - 1] = '\0';
    strncpy(g_status.title_id, detail.title_id, sizeof(g_status.title_id) - 1);
    g_status.title_id[sizeof(g_status.title_id) - 1] = '\0';
    strncpy(g_status.title_name, detail.title_name, sizeof(g_status.title_name) - 1);
    g_status.title_name[sizeof(g_status.title_name) - 1] = '\0';
    strncpy(g_status.content_id, detail.content_id, sizeof(g_status.content_id) - 1);
    g_status.content_id[sizeof(g_status.content_id) - 1] = '\0';
    strncpy(g_status.pkg_kind, detail.pkg_type_str, sizeof(g_status.pkg_kind) - 1);
    g_status.pkg_kind[sizeof(g_status.pkg_kind) - 1] = '\0';
    strncpy(g_status.pkg_version, detail.app_version, sizeof(g_status.pkg_version) - 1);
    g_status.pkg_version[sizeof(g_status.pkg_version) - 1] = '\0';
    strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
    g_status.status_str[sizeof(g_status.status_str) - 1] = '\0';
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
             "Installing %.200s...",
             g_status.title_name[0] != '\0' ? g_status.title_name : "Package");
    g_status.total_bytes = required_space;
    g_status.downloaded_bytes = 0;
    g_status.stream_served_bytes = 0;
    g_status.progress_percent = 0.0f;
    g_status.start_time = time(NULL);
    g_status.last_poll_time = time(NULL);
    g_cancel_stream = 0;

    if (pthread_create(&g_stream_thread, NULL, stream_installer_worker, NULL) != 0) {
        g_status.is_installing = 0;
        g_status.failed = 1;
        pthread_mutex_unlock(&g_installer_mutex);
        return -12;
    }
    g_stream_thread_created = 1;
    char notify_title[256];
    strncpy(notify_title, g_status.title_name[0] ? g_status.title_name : "Package",
            sizeof(notify_title) - 1);
    notify_title[sizeof(notify_title) - 1] = '\0';
    pthread_mutex_unlock(&g_installer_mutex);
    ps5_notify("Installing %s (live)...", notify_title);
    return 0;
}

int installer_cancel(void) {
    pthread_mutex_lock(&g_installer_mutex);
    if (!g_status.is_installing) {
        pthread_mutex_unlock(&g_installer_mutex);
        return -1;
    }
    g_cancel_stream = 1;
    g_status.is_installing = 0;
    g_status.failed = 1;
    strncpy(g_status.status_str, "canceled", sizeof(g_status.status_str) - 1);
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Installation was canceled");
    pthread_mutex_unlock(&g_installer_mutex);
    /* NEW: unblock live readers before the stop drains vs_refs, then free. */
    ws_live_abort();
    stream_server_session_stop();
    ws_live_destroy();
    ps5_notify("Installation canceled");
    return 0;
}

void installer_record_poll(void) {
    pthread_mutex_lock(&g_installer_mutex);
    g_status.last_poll_time = time(NULL);
    pthread_mutex_unlock(&g_installer_mutex);
}

void installer_get_status(installer_status_t *out) {
    if (!out) return;
    pthread_mutex_lock(&g_installer_mutex);
    memcpy(out, &g_status, sizeof(installer_status_t));
    pthread_mutex_unlock(&g_installer_mutex);
}

static void escape_json_str(const char *src, char *dst, size_t dst_max) {
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 2 < dst_max; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '"') {
            if (d + 2 >= dst_max) break;
            dst[d++] = '\\';
            dst[d++] = '"';
        } else if (c == '\\') {
            if (d + 2 >= dst_max) break;
            dst[d++] = '\\';
            dst[d++] = '\\';
        } else if (c == '\n') {
            if (d + 2 >= dst_max) break;
            dst[d++] = '\\';
            dst[d++] = 'n';
        } else if (c == '\r') {
            if (d + 2 >= dst_max) break;
            dst[d++] = '\\';
            dst[d++] = 'r';
        } else if (c == '\t') {
            if (d + 2 >= dst_max) break;
            dst[d++] = '\\';
            dst[d++] = 't';
        } else if (c < 32) {
            /* Skip control chars */
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

char *installer_status_to_json(void) {
    pthread_mutex_lock(&g_installer_mutex);
    g_status.last_poll_time = time(NULL); /* Heartbeat */

    char *json = (char *)malloc(4096);
    if (!json) {
        pthread_mutex_unlock(&g_installer_mutex);
        return NULL;
    }

    char esc_path[1024];
    char esc_title_id[64];
    char esc_title_name[512];
    char esc_content_id[128];
    char esc_status[64];
    char esc_prompt[512];

    escape_json_str(g_status.pkg_path, esc_path, sizeof(esc_path));
    escape_json_str(g_status.title_id, esc_title_id, sizeof(esc_title_id));
    escape_json_str(g_status.title_name, esc_title_name, sizeof(esc_title_name));
    escape_json_str(g_status.content_id, esc_content_id, sizeof(esc_content_id));
    escape_json_str(g_status.status_str, esc_status, sizeof(esc_status));
    escape_json_str(g_status.prompt_message, esc_prompt, sizeof(esc_prompt));

    snprintf(json, 4096,
        "{"
        "\"is_installing\":%s,"
        "\"pkg_path\":\"%s\","
        "\"title_id\":\"%s\","
        "\"title_name\":\"%s\","
        "\"content_id\":\"%s\","
        "\"status\":\"%s\","
        "\"downloaded_bytes\":%llu,"
        "\"total_bytes\":%llu,"
        "\"progress\":%.2f,"
        "\"error_code\":%d,"
        "\"completed\":%s,"
        "\"failed\":%s,"
        "\"is_multipart\":%s,"
        "\"current_part\":%u,"
        "\"total_parts\":%u,"
        "\"waiting_for_disc\":%s,"
        "\"prompt_message\":\"%s\""
        "}",
        g_status.is_installing ? "true" : "false",
        esc_path,
        esc_title_id,
        esc_title_name,
        esc_content_id,
        esc_status,
        (unsigned long long)g_status.downloaded_bytes,
        (unsigned long long)g_status.total_bytes,
        g_status.progress_percent,
        g_status.error_code,
        g_status.completed ? "true" : "false",
        g_status.failed ? "true" : "false",
        g_status.is_multipart ? "true" : "false",
        g_status.current_part,
        g_status.total_parts,
        g_status.waiting_for_disc ? "true" : "false",
        esc_prompt
    );

    pthread_mutex_unlock(&g_installer_mutex);
    return json;
}

void installer_shutdown(void) {
    g_cancel_stream = 1;
    g_monitor_running = 0;
    /* NEW: unblock any live readers so the worker join below can't wedge. */
    ws_live_abort();
    if (g_stream_thread_created) {
        pthread_join(g_stream_thread, NULL);
        g_stream_thread_created = 0;
    }
    if (g_monitor_thread_created) {
        pthread_join(g_monitor_thread, NULL);
        g_monitor_thread_created = 0;
    }
    stream_server_session_stop();
#if defined(__Prospero__) || defined(PS5_BUILD)
    sceAppInstUtilTerminate();
#endif
}

int system_get_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used) {
    if (!out_free || !out_total || !out_used) return -1;
    struct statvfs sv;
    const char *paths[] = {"/data", "/user", "/tmp", "/", NULL};
    for (int i = 0; paths[i] != NULL; i++) {
        if (statvfs(paths[i], &sv) == 0 && sv.f_blocks > 0) {
            uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
            *out_total = (uint64_t)sv.f_blocks * bsize;
            *out_free = (uint64_t)sv.f_bavail * bsize;
            *out_used = (*out_total >= *out_free) ? (*out_total - *out_free) : 0;
            return 0;
        }
    }
    *out_total = 667200000000ULL;
    *out_free  = 350000000000ULL;
    *out_used  = 317200000000ULL;
    return 0;
}

int system_get_nvme_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used) {
    if (!out_free || !out_total || !out_used) return -1;
    *out_free = 0;
    *out_total = 0;
    *out_used = 0;

    const char *env_path = getenv("PKG_EXT1_DIR");
    const char *ext_path = (env_path && env_path[0] != '\0') ? env_path : "/mnt/ext1";

    if (getenv("PKG_FORCE_NVME_SPACE_FAIL")) {
        *out_total = 1024 * 1024 * 1024ULL;
        *out_free = 1024ULL;
        *out_used = *out_total - *out_free;
        return 0;
    }

    struct stat st_ext;
    if (stat(ext_path, &st_ext) != 0) {
        return -1;
    }

    /* If default path /mnt/ext1, verify it is a distinct mount, not an unmounted folder on root */
    if (!env_path) {
        struct stat st_parent;
        if (stat("/mnt", &st_parent) == 0) {
            if (st_ext.st_dev == st_parent.st_dev) {
                return -1;
            }
        } else if (stat("/", &st_parent) == 0) {
            if (st_ext.st_dev == st_parent.st_dev) {
                return -1;
            }
        }
    }

    struct statvfs sv;
    if (statvfs(ext_path, &sv) == 0 && sv.f_blocks > 0) {
        uint64_t bsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        *out_total = (uint64_t)sv.f_blocks * bsize;
        *out_free = (uint64_t)sv.f_bavail * bsize;
        *out_used = (*out_total >= *out_free) ? (*out_total - *out_free) : 0;
        return 0;
    }

    return -1;
}

