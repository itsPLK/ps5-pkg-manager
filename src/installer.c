/*
 * PKG Manager - Package Installation Engine
 *
 * Coordinates package streaming, multi-part verification,
 * AppInstUtil integration, and background installation progress.
 */

#include "installer.h"
#include "split_pkg.h"
#include "install_queue.h"
#include "install_service.h"
#include "pkg_parser.h"
#include "pkg_scanner.h"
#include "multipart.h"
#include "notification.h"
#include "app_info.h"
#include "stream_server.h"
#include "stream_debug_log.h"
#include "pkg_cache.h"
#include "ws_stream.h" /* NEW: live RAM sessions (additive; worker below unchanged) */
#include "ws_upload.h"

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
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#if defined(__Prospero__) || defined(PS5_BUILD)
extern int sceNetCtlGetState(int *state);
#endif


static installer_status_t g_status;
static pthread_mutex_t g_installer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_monitor_thread;
static volatile int g_monitor_running = 0;
static volatile int g_monitor_thread_created = 0;
static pthread_t g_stream_thread;
static volatile int g_stream_thread_created = 0;
static volatile int g_cancel_stream = 0;
static int g_worker_busy, g_cancel_cleanup;
static pthread_mutex_t g_start_mutex = PTHREAD_MUTEX_INITIALIZER;
static int installer_start_internal(const char *pkg_path);

static void installer_submitted(void) {
    pthread_mutex_lock(&g_installer_mutex);
    g_status.is_installing = 0;
    g_status.completed = 0;
    g_status.failed = 0;
    g_status.progress_percent = -1;
    g_status.is_direct_storage = 1;
    snprintf(g_status.status_str, sizeof(g_status.status_str), "submitted");
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Submitted to PS5. Track progress and cancellation in PS5 Notifications.");
    pthread_mutex_unlock(&g_installer_mutex);
    install_log("[INSTALLER] Submitted to PS5 system installer");
}

/* Caller holds g_installer_mutex. */
static void installer_complete_locked(void) {
    g_status.downloaded_bytes = g_status.total_bytes;
    g_status.progress_percent = 100;
    g_status.is_installing = 0;
    g_status.completed = 1;
    snprintf(g_status.status_str, sizeof(g_status.status_str), "playable");
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "%s is ready to play!",
             g_status.title_name[0] ? g_status.title_name : "Package");
}

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

static int title_id_has_prefix(const char *title_id, const char *prefix) {
    return title_id && prefix && strncasecmp(title_id, prefix, 4) == 0;
}

static int package_is_ps4(const pkg_detail_t *detail) {
    return detail && title_id_has_prefix(detail->title_id, "CUSA");
}

/* Validate the destinations the console can use for this package. PS4 titles
 * can use USB extended storage; PS5 titles are restricted to internal/M.2. */
static int validate_install_storage(const pkg_detail_t *detail, uint64_t required_space) {
    uint64_t nvme_f = 0, nvme_t = 0, nvme_u = 0;
    uint64_t usb_f = 0, usb_t = 0, usb_u = 0;
    int has_nvme = (system_get_nvme_storage_info(&nvme_f, &nvme_t, &nvme_u) == 0);
    int has_usb = package_is_ps4(detail) &&
                  (system_get_usb_storage_info(&usb_f, &usb_t, &usb_u) == 0);

    const char *check_dir = getenv("PKG_TMP_DIR");
    if (!check_dir || check_dir[0] == '\0') check_dir = "/data";

    uint64_t internal_f = get_available_disk_space(check_dir);
    int has_internal = (internal_f != (uint64_t)-1);
    uint64_t max_avail = has_internal ? internal_f : 0;
    if (has_nvme && nvme_f > max_avail) max_avail = nvme_f;
    if (has_usb && usb_f > max_avail) max_avail = usb_f;

    if ((has_internal || has_nvme || has_usb) && max_avail < required_space) {
        if (package_is_ps4(detail)) {
            ps5_notify("Not enough storage space! Need %llu MB (Internal: %llu MB, M.2: %llu MB, USB: %llu MB)",
                       (unsigned long long)(required_space / (1024 * 1024)),
                       (unsigned long long)((has_internal ? internal_f : 0) / (1024 * 1024)),
                       (unsigned long long)((has_nvme ? nvme_f : 0) / (1024 * 1024)),
                       (unsigned long long)((has_usb ? usb_f : 0) / (1024 * 1024)));
        } else if (has_nvme) {
            ps5_notify("Not enough storage space! Need %llu MB (Internal: %llu MB, M.2: %llu MB)",
                       (unsigned long long)(required_space / (1024 * 1024)),
                       (unsigned long long)((has_internal ? internal_f : 0) / (1024 * 1024)),
                       (unsigned long long)(nvme_f / (1024 * 1024)));
        } else {
            ps5_notify("Not enough storage space! Need %llu MB, have %llu MB",
                       (unsigned long long)(required_space / (1024 * 1024)),
                       (unsigned long long)((has_internal ? internal_f : 0) / (1024 * 1024)));
        }
        return -10;
    }

    return 0;
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

/* Keep recent diagnostics without reserving tens of MiB in the daemon. */
#define MAX_LOG_LINES INSTALL_LOG_MAX_LINES
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

void installer_notify_source_error(const char *expected_path) {
    pthread_mutex_lock(&g_installer_mutex);
    if (expected_path && g_status.is_installing && !g_status.is_direct_storage &&
        !strcmp(g_status.pkg_path, expected_path)) {
        g_cancel_stream = 1;
        g_status.is_installing = 0;
        g_status.failed = 1;
        g_status.error_code = -4;
        snprintf(g_status.status_str, sizeof(g_status.status_str), "error");
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Package source stopped providing data. Reconnect the drive/share and retry.");
        install_log("[INSTALLER] Package source read failed: %.160s", expected_path);
    }
    pthread_mutex_unlock(&g_installer_mutex);
    /* The installer worker performs cleanup. Joining the stream session
     * from one of its readers would deadlock on that reader's reference. */
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

/* Human-readable names for installer/playgo error codes (verified against
   etaHEN error_translator and on-console results). Unknown codes -> NULL. */
const char *installer_strerror(int code) {
    if (code == 0) {
        return "OK";
    }
    switch (code) {
    case INSTALL_SERVICE_UNAVAILABLE: return "INSTALL_HELPER_UNAVAILABLE";
    case INSTALL_SERVICE_DISCONNECTED: return "INSTALL_HELPER_DISCONNECTED";
    case INSTALL_SERVICE_CANCELED: return "INSTALL_HELPER_CANCELED";
    case INSTALL_SERVICE_TIMEOUT: return "INSTALL_HELPER_TIMEOUT";
    }
    switch ((uint32_t)code) {
    case 0x80A30001u: return "APP_INSTALLER_ERROR_UNKNOWN";
    case 0x80A30002u: return "APP_INSTALLER_ERROR_NOSPACE";
    case 0x80A30003u: return "APP_INSTALLER_ERROR_PARAM";
    case 0x80B21104u: return "SCE_PLAYGO_ERROR_CORE_NO_FREE_SPACE";
    case 0x80B21121u: return "SCE_PLAYGO_ERROR_CORE_NET_NOT_CONNECTED";
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

int installer_is_nospace_error(int code) {
    uint32_t c = (uint32_t)code;
    return c == 0x80B21104u || c == 0x80A30002u;
}

#if defined(__Prospero__) || defined(PS5_BUILD)

/* Slot-family errors are transient (e.g. patch installed while the system
   still finalizes the base): safe to retry with a fresh session. Anything
   else, including PARAM, fails immediately. */
static int install_request_canceled(void) {
    return g_cancel_stream || !g_monitor_running;
}

static int is_transient_slot_error(int code) {
    uint32_t c = (uint32_t)code;
    return c == 0x80B2116Fu || c == 0x80B2100Du || c == 0x80B2100Eu;
}

static int check_package_verified_installed(const char *title_id, const char *kind,
                                           const char *content_id, const char *expect_ver) {
    if (!title_id || title_id[0] == '\0') return 0;
    if ((kind && strcasecmp(kind, "dlc") == 0) && content_id && content_id[0] != '\0') {
        if (app_info_check_dlc_installed(title_id, content_id)) {
            install_log("[INSTALLER] DLC verified installed: %s / %s", title_id, content_id);
            return 1;
        }
    } else if (expect_ver && expect_ver[0] != '\0') {
        char installed_ver[32] = {0};
        if (app_info_check_installed(title_id, installed_ver, sizeof(installed_ver))) {
            if (installed_ver[0] != '\0' &&
                app_info_compare_versions(installed_ver, expect_ver) >= 0) {
                install_log("[INSTALLER] %s verified installed: %s %s >= %s",
                            (kind && strcasecmp(kind, "update") == 0) ? "Update" : "App",
                            title_id, installed_ver, expect_ver);
                return 1;
            } else if (installed_ver[0] != '\0') {
                install_log("[INSTALLER] %s not yet applied: %s installed=%s expect=%s",
                            (kind && strcasecmp(kind, "update") == 0) ? "Update" : "App",
                            title_id, installed_ver, expect_ver);
            } else if (!kind || strcasecmp(kind, "update") != 0) {
                install_log("[INSTALLER] App verified installed in database: %s", title_id);
                return 1;
            }
        }
    } else {
        if (app_info_check_installed(title_id, NULL, 0)) {
            install_log("[INSTALLER] App verified installed in database: %s", title_id);
            return 1;
        }
    }
    return 0;
}
#endif

static int is_usb_or_disc_path(const char *path) {
    if (!path || path[0] == '\0') return 0;
    if (strncmp(path, "smb://", 6) == 0) return 0;
    if (strncmp(path, "live:", 5) == 0) return 0;
    /* A numbered piece set only exists as a whole through the stream server;
     * handing piece 0 straight to the system installer would be truncated. */
    if (split_pkg_detect(path, NULL)) return 0;

    const char *usb_prefix = getenv("PKG_USB_PREFIX");
    if (usb_prefix && usb_prefix[0] != '\0') {
        if (strncmp(path, usb_prefix, strlen(usb_prefix)) == 0) return 1;
    }
    if (strncmp(path, "/mnt/usb", 8) == 0) return 1;

    const char *disc_dir = getenv("PKG_DISC_DIR");
    if (disc_dir && disc_dir[0] != '\0') {
        if (strncmp(path, disc_dir, strlen(disc_dir)) == 0) return 1;
    }
    if (strncmp(path, "/mnt/disc", 9) == 0) return 1;

    /* Internal storage source (/data/pkg) behaves like a local drive */
    const char *internal_dir = getenv("PKG_INTERNAL_DIR");
    if (internal_dir && internal_dir[0] != '\0') {
        size_t ilen = strlen(internal_dir);
        if (strncmp(path, internal_dir, ilen) == 0 &&
            (path[ilen] == '/' || path[ilen] == '\0' || internal_dir[ilen - 1] == '/')) return 1;
    }
    if (strncmp(path, "/data/pkg/", 10) == 0) return 1;

    const char *scan_dir = getenv("PKG_SCAN_DIR");
    if (scan_dir && scan_dir[0] != '\0') {
        if (strncmp(path, scan_dir, strlen(scan_dir)) == 0) return 1;
    }

    return 0;
}

int installer_is_filesystem_install(const char *pkg_path, int is_multipart) {
    return is_usb_or_disc_path(pkg_path) && !is_multipart;
}

static volatile int s_cached_offline = 0;
static time_t s_cached_offline_time = 0;

void installer_set_network_offline_cached(int offline) {
    s_cached_offline = offline;
    s_cached_offline_time = offline ? time(NULL) : 0;
}

static int has_active_ipv4_interface(void) {
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }
    int found = 0;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;

        char ip_buf[INET_ADDRSTRLEN] = {0};
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf))) {
            if (strcmp(ip_buf, "127.0.0.1") != 0 && strcmp(ip_buf, "0.0.0.0") != 0) {
                found = 1;
                break;
            }
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

int installer_is_network_connected(void) {
#if defined(__Prospero__) || defined(PS5_BUILD)
    if (s_cached_offline) {
        if (time(NULL) - s_cached_offline_time < 300) {
            install_log("[INSTALLER] Cached PlayGo offline state active");
            return 0;
        }
        s_cached_offline = 0;
    }

    int state = 0;
    int ret = sceNetCtlGetState(&state);
    if (ret == 0) {
        /* 0=DISCONNECTED, 1=CONNECTING, 2=IPOBTAINING, 3=IPOBTAINED */
        if (state != 3) {
            install_log("[INSTALLER] SceNetCtl reports network state %d (not connected)", state);
            return 0;
        }
    }

    if (!has_active_ipv4_interface()) {
        install_log("[INSTALLER] No active non-loopback IPv4 interface detected");
        return 0;
    }

    return 1;
#else
    if (getenv("PKG_TEST_OFFLINE_NET") != NULL) {
        return 0;
    }
    if (s_cached_offline) {
        if (time(NULL) - s_cached_offline_time < 300) {
            return 0;
        }
        s_cached_offline = 0;
    }
    return 1;
#endif
}

static int icon_has_png_magic(const uint8_t *data, size_t size) {
    static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || size < 8) return 0;
    return memcmp(data, png_magic, sizeof(png_magic)) == 0;
}

static uint8_t *extract_pkg_icon(const char *pkg_path, int is_multipart,
                                 const multipart_header_t *hdr1, const char *title_id,
                                 size_t *out_size) {
    if (!pkg_path || !out_size) return NULL;
    *out_size = 0;

    /* 1. Live upload session (RAM) */
    if (strncmp(pkg_path, "live:", 5) == 0) {
        uint8_t *icon_data = NULL;
        size_t icon_size = 0;
        if (ws_direct_get_icon(pkg_path + 5, &icon_data, &icon_size) == 0 &&
            icon_data && icon_size > 0 && icon_has_png_magic(icon_data, icon_size)) {
            *out_size = icon_size;
            return icon_data;
        }
        free(icon_data);
    }

    /* 2. Multipart package Part 1 header */
    if (is_multipart && hdr1 && hdr1->icon_offset > 0 && hdr1->icon_size > 0) {
        uint8_t *buf = NULL;
        size_t sz = 0;
        if (pkg_parser_get_icon(pkg_path, hdr1->icon_offset, hdr1->icon_size, &buf, &sz) == 0 &&
            buf && sz > 0 && icon_has_png_magic(buf, sz)) {
            *out_size = sz;
            return buf;
        }
        free(buf);
    }

    /* 3. Disk/SMB metadata cache */
    if (strncmp(pkg_path, "live:", 5) != 0) {
        uint8_t *cached_icon = NULL;
        size_t cached_sz = 0;
        if (pkg_cache_get_icon(pkg_path, &cached_icon, &cached_sz) == 0 &&
            cached_icon && cached_sz > 0 && icon_has_png_magic(cached_icon, cached_sz)) {
            *out_size = cached_sz;
            return cached_icon;
        }
        free(cached_icon);
    }

    /* 4. Scanner cache / lookup */
    if (strncmp(pkg_path, "live:", 5) != 0) {
        pkg_detail_t detail;
        if (pkg_scanner_find_by_path(pkg_path, &detail) == 0 &&
            detail.has_icon && detail.icon_offset > 0 && detail.icon_size > 0) {
            uint8_t *buf = NULL;
            size_t sz = 0;
            if (pkg_parser_get_icon(detail.path, detail.icon_offset, detail.icon_size, &buf, &sz) == 0 &&
                buf && sz > 0 && icon_has_png_magic(buf, sz)) {
                *out_size = sz;
                return buf;
            }
            free(buf);
        }
    }

    /* 5. Fresh parse (if local file or smb not yet in scanner) */
    if (strncmp(pkg_path, "live:", 5) != 0) {
        pkg_detail_t detail;
        if (pkg_parser_parse(pkg_path, &detail) == 0 &&
            detail.has_icon && detail.icon_offset > 0 && detail.icon_size > 0) {
            uint8_t *buf = NULL;
            size_t sz = 0;
            if (pkg_parser_get_icon(detail.path, detail.icon_offset, detail.icon_size, &buf, &sz) == 0 &&
                buf && sz > 0 && icon_has_png_magic(buf, sz)) {
                *out_size = sz;
                return buf;
            }
            free(buf);
        }
    }

    /* 6. Fallback for updates/patches without icon: check installed app icon */
    if (title_id && title_id[0] != '\0') {
        char app_icon_path[256];
        snprintf(app_icon_path, sizeof(app_icon_path), "/user/app/%s/sce_sys/icon0.png", title_id);
        FILE *f = fopen(app_icon_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            if (fsz > 8 && fsz < 10 * 1024 * 1024) {
                fseek(f, 0, SEEK_SET);
                uint8_t *ibuf = (uint8_t *)malloc((size_t)fsz);
                if (ibuf) {
                    if (fread(ibuf, 1, (size_t)fsz, f) == (size_t)fsz && icon_has_png_magic(ibuf, (size_t)fsz)) {
                        fclose(f);
                        *out_size = (size_t)fsz;
                        return ibuf;
                    }
                    free(ibuf);
                }
            }
            fclose(f);
        }
    }

    return NULL;
}

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

    int is_filesystem_install = 0;
    int can_fallback_to_direct = is_usb_or_disc_path(worker_pkg_path) && !worker_is_multipart;

    if (can_fallback_to_direct && !installer_is_network_connected()) {
        install_log("[INSTALLER] Network not connected; using direct storage install for %s", worker_pkg_path);
        is_filesystem_install = 1;
        can_fallback_to_direct = 0;
    }

    /* Activate stream debug file logging if the setting is enabled.
     * Open before validation/launch so early failures are captured too. */
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
            stream_debug_log_close();
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
            install_log("[INSTALLER] Invalid total parts count (%u)", total_parts);
            pthread_mutex_unlock(&g_installer_mutex);
            stream_debug_log_close();
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

    /* Display name for the system installer UI. The helper retains a copy
       for the native session's lifetime. STREAM_NAME_OVERRIDE (build-time
       -D) pins one literal for A/B runs; otherwise app title + kind
       (e.g. "<title> (base)", "<title> (update v<ver>)", or "<title> (DLC)"). */
    static char disp_name[320];
#ifdef STREAM_NAME_OVERRIDE
    strncpy(disp_name, STREAM_NAME_OVERRIDE, sizeof(disp_name) - 1);
    disp_name[sizeof(disp_name) - 1] = '\0';
#else
    {
        char title_copy[256] = {0};
        char kind_copy[16] = "base";
        char ver_copy[32] = {0};
        pthread_mutex_lock(&g_installer_mutex);
        if (worker_is_multipart && hdr1.title_name[0] != '\0') {
            strncpy(title_copy, hdr1.title_name, sizeof(title_copy) - 1);
            title_copy[sizeof(title_copy) - 1] = '\0';
        } else if (g_status.title_name[0] != '\0') {
            strncpy(title_copy, g_status.title_name, sizeof(title_copy) - 1);
            title_copy[sizeof(title_copy) - 1] = '\0';
        }
        if (worker_is_multipart && hdr1.pkg_type[0] != '\0') {
            strncpy(kind_copy, hdr1.pkg_type, sizeof(kind_copy) - 1);
            kind_copy[sizeof(kind_copy) - 1] = '\0';
        } else if (g_status.pkg_kind[0] != '\0') {
            strncpy(kind_copy, g_status.pkg_kind, sizeof(kind_copy) - 1);
            kind_copy[sizeof(kind_copy) - 1] = '\0';
        }
        if (worker_is_multipart && hdr1.app_version[0] != '\0') {
            strncpy(ver_copy, hdr1.app_version, sizeof(ver_copy) - 1);
            ver_copy[sizeof(ver_copy) - 1] = '\0';
        } else if (g_status.pkg_version[0] != '\0') {
            strncpy(ver_copy, g_status.pkg_version, sizeof(ver_copy) - 1);
            ver_copy[sizeof(ver_copy) - 1] = '\0';
        }
        pthread_mutex_unlock(&g_installer_mutex);

        if (title_copy[0] == '\0') {
            if (clean_pkg_name[0] != '\0') {
                strncpy(title_copy, clean_pkg_name, sizeof(title_copy) - 1);
                title_copy[sizeof(title_copy) - 1] = '\0';
                size_t clen = strlen(title_copy);
                if (clen > 4 && strcasecmp(title_copy + clen - 4, ".pkg") == 0) {
                    title_copy[clen - 4] = '\0';
                }
            } else {
                strncpy(title_copy, "Package", sizeof(title_copy) - 1);
                title_copy[sizeof(title_copy) - 1] = '\0';
            }
        }

        if (strcasecmp(kind_copy, "update") == 0) {
            const char *vptr = ver_copy;
            while (*vptr == ' ' || *vptr == '\t') vptr++;
            if (*vptr == 'v' || *vptr == 'V') vptr++;
            while (*vptr == ' ' || *vptr == '\t') vptr++;
            if (vptr[0] != '\0') {
                snprintf(disp_name, sizeof(disp_name), "%s (update v%s)", title_copy, vptr);
            } else {
                snprintf(disp_name, sizeof(disp_name), "%s (update)", title_copy);
            }
        } else if (strcasecmp(kind_copy, "dlc") == 0) {
            snprintf(disp_name, sizeof(disp_name), "%s (DLC)", title_copy);
        } else {
            snprintf(disp_name, sizeof(disp_name), "%s (base)", title_copy);
        }
    }
#endif

    /* Start the raw-socket stream server for the system installer (stream installs only). */
    if (!is_filesystem_install) {
        if (stream_server_session_start(worker_pkg_path) != 0) {
            pthread_mutex_lock(&g_installer_mutex);
            g_status.is_installing = 0;
            g_status.failed = 1;
            g_status.error_code = -23;
            strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Package file not found or cannot be opened");
            pthread_mutex_unlock(&g_installer_mutex);
            ps5_notify("Package file not found or cannot be opened!");
            install_log("[INSTALLER] Failed to open stream for %s (file missing or inaccessible)", worker_pkg_path);
            stream_debug_log_close();
            return NULL;
        }
    }

    /* Unique URI per install: the system remembers recently used stream URLs
       across payload restarts (reusing package-1.pkg after a redeploy gets
       rejected), so key by wall-clock timestamp plus a per-install sequence
       for same-second safety. The pinned basename is published to the raw
       server, which serves only that exact name (anything else 404s). */
    static unsigned int s_stream_seq = 0;
    char target_uri[1024];
    char icon_uri[1024] = {0};
    uint8_t *extracted_icon = NULL;
    size_t extracted_icon_size = 0;

    char tid[32] = {0};
    pthread_mutex_lock(&g_installer_mutex);
    if (g_status.title_id[0] != '\0') {
        strncpy(tid, g_status.title_id, sizeof(tid) - 1);
    }
    pthread_mutex_unlock(&g_installer_mutex);
    if (tid[0] == '\0' && hdr1.title_id[0] != '\0') {
        strncpy(tid, hdr1.title_id, sizeof(tid) - 1);
    }
    if (is_filesystem_install) {
        snprintf(target_uri, sizeof(target_uri), "%s", worker_pkg_path);
        icon_uri[0] = '\0';
    } else {
        snprintf(target_uri, sizeof(target_uri), "http://127.0.0.1:%d/stream/install/package-%lu-%u.pkg",
                 STREAM_SERVER_PORT, (unsigned long)time(NULL), (unsigned int)++s_stream_seq);
        /* Pin the exact session filename so the stream server 404s any
         * foreign name (e.g. client-derived <content_id>.crc sidecars). */
        {
            const char *slash = strrchr(target_uri, '/');
            stream_server_set_session_name(slash ? slash + 1 : target_uri);
        }

        extracted_icon = extract_pkg_icon(worker_pkg_path, worker_is_multipart,
                                          &hdr1, tid, &extracted_icon_size);
        char icon_filename[128] = {0};
        if (extracted_icon && extracted_icon_size > 0) {
            snprintf(icon_filename, sizeof(icon_filename), "icon-%lu-%u.png",
                     (unsigned long)time(NULL), s_stream_seq);
            snprintf(icon_uri, sizeof(icon_uri), "http://127.0.0.1:%d/stream/install/%s",
                     STREAM_SERVER_PORT, icon_filename);
            stream_server_set_icon(extracted_icon, extracted_icon_size, icon_filename);
        }
    }

    pthread_mutex_lock(&g_installer_mutex);
    if (g_cancel_stream || !g_monitor_running) {
        pthread_mutex_unlock(&g_installer_mutex);
        install_log("[INSTALLER] Canceled before helper launch");
        free(extracted_icon);
        ws_live_abort();
        if (!is_filesystem_install) {
            stream_server_session_stop();
        }
        ws_live_destroy();
        return NULL;
    }
    g_status.waiting_for_disc = 0;
    if (is_filesystem_install) {
        g_status.is_direct_storage = 1;
        g_status.downloaded_bytes = 0;
        g_status.progress_percent = -1.0f;
        strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing via direct storage. Track progress in the PS5 home menu. (Tip: connect to any network to see detailed real-time progress here).");
    } else {
        g_status.is_direct_storage = 0;
        strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing package %.200s...", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
        g_status.downloaded_bytes = 0;
        g_status.stream_served_bytes = 0;
        if (g_status.is_multipart) {
            g_status.total_bytes = hdr1.total_pkg_size;
        }
        g_status.progress_percent = 0.0f;
    }
    pthread_mutex_unlock(&g_installer_mutex);

    if (is_filesystem_install) {
        ps5_notify("Direct storage install started. Track progress in PS5 menu.");
    }

    if (is_filesystem_install && worker_pkg_path[0] != '\0') {
        pthread_mutex_lock(&g_installer_mutex);
        if (g_status.total_bytes == 0) {
            struct stat pkg_st;
            if (stat(worker_pkg_path, &pkg_st) == 0 && pkg_st.st_size > 0) {
                g_status.total_bytes = (uint64_t)pkg_st.st_size;
            }
        }
        pthread_mutex_unlock(&g_installer_mutex);
    }

    if (is_filesystem_install) {
        install_log("[INSTALLER] Initiating direct filesystem install: path='%s', name='%s', total_bytes=%llu",
                    target_uri, disp_name, (unsigned long long)g_status.total_bytes);
    } else {
        install_log("[INSTALLER] Initiating %sstream install: URI='%s', name='%s', icon='%s', total_bytes=%llu, parts=%u",
                    worker_is_multipart ? "multi-part " : "",
                    target_uri, disp_name, icon_uri, (unsigned long long)hdr1.total_pkg_size, total_parts);
    }

#if defined(__Prospero__) || defined(PS5_BUILD)
    install_service_t service = INSTALL_SERVICE_INIT;
    pkg_info_t info = {0};

    /* Retry slot-family errors with a fresh helper process and stream URI
       (initial try, then after 2s, then after 5s). Anything else, including
       PARAM, fails immediately. Waits abort promptly on cancel/shutdown. */
    static const int retry_delays[] = { 0, 2, 5 };
    int ret = -1;
    const char *rname = NULL;
    for (int attempt = 0; attempt < 3 && !g_cancel_stream; attempt++) {
        if (attempt > 0) {
            install_service_close(&service);
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
            if (!is_filesystem_install) {
                stream_server_session_stop_keep_log();
                if (stream_server_session_start(worker_pkg_path) != 0) {
                    install_log("[INSTALLER] Stream server restart failed; keeping last error");
                    break;
                }
                snprintf(target_uri, sizeof(target_uri), "http://127.0.0.1:%d/stream/install/package-%lu-%u.pkg",
                         STREAM_SERVER_PORT, (unsigned long)time(NULL), (unsigned int)++s_stream_seq);
                {
                    const char *slash = strrchr(target_uri, '/');
                    stream_server_set_session_name(slash ? slash + 1 : target_uri);
                }
                if (extracted_icon && extracted_icon_size > 0) {
                    char icon_filename[128];
                    snprintf(icon_filename, sizeof(icon_filename), "icon-%lu-%u.png",
                             (unsigned long)time(NULL), s_stream_seq);
                    snprintf(icon_uri, sizeof(icon_uri), "http://127.0.0.1:%d/stream/install/%s",
                             STREAM_SERVER_PORT, icon_filename);
                    stream_server_set_icon(extracted_icon, extracted_icon_size, icon_filename);
                } else {
                    icon_uri[0] = '\0';
                }

                install_log("[INSTALLER] Retry stream install: URI='%s', icon='%s'", target_uri, icon_uri);
            } else {
                install_log("[INSTALLER] Retry filesystem install: path='%s'", target_uri);
            }
        }
        ret = install_service_start(&service, target_uri, disp_name, icon_uri, &info,
                                    install_request_canceled);
        rname = installer_strerror(ret);
        install_log("[INSTALLER] Helper pid=%d install returned 0x%08X (%s), content_id='%s'",
                    (int)service.pid, ret, rname ? rname : "unknown", info.content_id);
        if (ret == 0 || !is_transient_slot_error(ret)) {
            break;
        }
    }
    rname = installer_strerror(ret);

    if (g_cancel_stream || !g_monitor_running) {
        /* Canceled or shutting down during retry waits: cancel/shutdown owns
           the status, just stop the server and exit. */
        install_log("[INSTALLER] Stopping helper after cancel/shutdown");
        install_service_close(&service);
        free(extracted_icon);
        ws_live_abort();
        if (!is_filesystem_install) {
            stream_server_session_stop();
        }
        ws_live_destroy();
        return NULL;
    }

    if ((uint32_t)ret == 0x80B21121u && can_fallback_to_direct && !g_cancel_stream && g_monitor_running) {
        install_log("[INSTALLER] Network not connected (0x80B21121); falling back to direct storage install for %s", worker_pkg_path);
        installer_set_network_offline_cached(1);
        install_service_close(&service);
        stream_server_session_stop();
        is_filesystem_install = 1;
        can_fallback_to_direct = 0;
        snprintf(target_uri, sizeof(target_uri), "%s", worker_pkg_path);
        icon_uri[0] = '\0';

        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_direct_storage = 1;
        g_status.downloaded_bytes = 0;
        g_status.progress_percent = -1.0f;
        if (g_status.total_bytes == 0) {
            struct stat pkg_st;
            if (stat(worker_pkg_path, &pkg_st) == 0 && pkg_st.st_size > 0) {
                g_status.total_bytes = (uint64_t)pkg_st.st_size;
            }
        }
        strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing via direct storage. Track progress in the PS5 home menu. (Tip: connect to any network to see detailed real-time progress here).");
        pthread_mutex_unlock(&g_installer_mutex);
        ps5_notify("Direct storage install started. Track progress in PS5 menu.");

        ret = install_service_start(&service, target_uri, disp_name, "", &info,
                                    install_request_canceled);
        rname = installer_strerror(ret);
        install_log("[INSTALLER] Direct storage helper pid=%d install returned 0x%08X (%s), content_id='%s'",
                    (int)service.pid, ret, rname ? rname : "unknown", info.content_id);


    }

    if (ret != 0) {
        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_installing = 0;
        g_status.failed = 1;
        g_status.error_code = ret;
        strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
        if (installer_is_nospace_error(ret)) {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Not enough free space for installation (0x%08X)", (unsigned)ret);
        } else {
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                     "Install failed: %s (0x%08X)", rname ? rname : "unknown", (unsigned)ret);
        }
        pthread_mutex_unlock(&g_installer_mutex);
        install_service_close(&service);
        if (installer_is_nospace_error(ret)) {
            ps5_notify("Not enough free space for installation");
        } else {
            ps5_notify("Install error: 0x%08X (%s)", ret, rname ? rname : "unknown");
        }
        free(extracted_icon);
        ws_live_abort();
        if (!is_filesystem_install) {
            stream_server_session_stop();
        }
        ws_live_destroy();
        return NULL;
    }

    pthread_mutex_lock(&g_installer_mutex);
    if (info.content_id[0] != '\0') {
        strncpy(g_status.content_id, info.content_id, sizeof(g_status.content_id) - 1);
        if (g_status.title_id[0] == '\0' || strcmp(g_status.title_id, "UNKNOWN") == 0) {
            const char *dash = strchr(info.content_id, '-');
            if (dash) {
                const char *tid_start = dash + 1;
                const char *underscore = strchr(tid_start, '_');
                if (underscore && (size_t)(underscore - tid_start) < sizeof(g_status.title_id)) {
                    size_t tlen = (size_t)(underscore - tid_start);
                    strncpy(g_status.title_id, tid_start, tlen);
                    g_status.title_id[tlen] = '\0';
                    install_log("[INSTALLER] Recovered title_id '%s' from content_id '%s'",
                                g_status.title_id, info.content_id);
                }
            }
        }
    }
    pthread_mutex_unlock(&g_installer_mutex);

    if (is_filesystem_install) {
        installer_submitted();
        install_service_close(&service);
        free(extracted_icon);
        return NULL;
    }

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

        /* Always poll the helper, including when the service supplied no ID,
         * so a dead helper cannot be mistaken for ongoing installation. */
        int is_sys_done = 0;
        {
            SceAppInstallStatusInstalled sys_status;
            memset(&sys_status, 0, sizeof(sys_status));
            int status_ret = install_service_status(&service, &sys_status);
            if (status_ret == INSTALL_SERVICE_CANCELED) break;
            if (status_ret == INSTALL_SERVICE_DISCONNECTED || status_ret == INSTALL_SERVICE_TIMEOUT) {
                install_log("[INSTALLER] Lost helper pid=%d (code %d)", (int)service.pid, status_ret);
                pthread_mutex_lock(&g_installer_mutex);
                if (!g_cancel_stream && g_monitor_running) {
                    g_status.is_installing = 0;
                    g_status.failed = 1;
                    g_status.error_code = status_ret;
                    strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
                    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                             "Install helper stopped responding (code %d)", status_ret);
                }
                pthread_mutex_unlock(&g_installer_mutex);
                break;
            }
            if (status_ret == 0) {
                if (sys_status.error_info.error_code != 0 || strcmp(sys_status.status, "error") == 0 || strcmp(sys_status.status, "none") == 0) {
                    if (!is_filesystem_install && can_fallback_to_direct &&
                        (uint32_t)sys_status.error_info.error_code == 0x80B21121u) {
                        install_log("[INSTALLER] Network not connected (0x80B21121) during status poll; falling back to direct storage install for %s", worker_pkg_path);
                        installer_set_network_offline_cached(1);
                        install_service_close(&service);
                        stream_server_session_stop();
                        is_filesystem_install = 1;
                        can_fallback_to_direct = 0;
                        snprintf(target_uri, sizeof(target_uri), "%s", worker_pkg_path);
                        icon_uri[0] = '\0';

                        pthread_mutex_lock(&g_installer_mutex);
                        g_status.is_direct_storage = 1;
                        g_status.downloaded_bytes = 0;
                        g_status.progress_percent = -1.0f;
                        if (g_status.total_bytes == 0) {
                            struct stat pkg_st;
                            if (stat(worker_pkg_path, &pkg_st) == 0 && pkg_st.st_size > 0) {
                                g_status.total_bytes = (uint64_t)pkg_st.st_size;
                            }
                        }
                        strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
                        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                 "Installing via direct storage. Track progress in the PS5 home menu. (Tip: connect to any network to see detailed real-time progress here).");
                        pthread_mutex_unlock(&g_installer_mutex);
                        ps5_notify("Direct storage install started. Track progress in PS5 menu.");

                        int fb_ret = install_service_start(&service, target_uri, disp_name, "", &info,
                                                           install_request_canceled);
                        install_log("[INSTALLER] Direct storage helper pid=%d install returned 0x%08X, content_id='%s'",
                                    (int)service.pid, fb_ret, info.content_id);
                        if (fb_ret != 0) {
                            pthread_mutex_lock(&g_installer_mutex);
                            g_status.is_installing = 0;
                            g_status.failed = 1;
                            g_status.error_code = fb_ret;
                            strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
                            if (installer_is_nospace_error(fb_ret)) {
                                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                         "Not enough free space for installation (0x%08X)", (unsigned)fb_ret);
                                ps5_notify("Not enough free space for installation");
                            } else {
                                snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                         "Direct storage install failed: 0x%08X", (unsigned)fb_ret);
                            }
                            pthread_mutex_unlock(&g_installer_mutex);
                            break;
                        }
                        installer_submitted();
                        break;
                    }
                    const char *sname = installer_strerror(sys_status.error_info.error_code);
                    install_log("[INSTALLER] System installer reported error 0x%08X (%s) (status='%s')",
                                sys_status.error_info.error_code, sname ? sname : "unknown", sys_status.status);
                    pthread_mutex_lock(&g_installer_mutex);
                    g_status.is_installing = 0;
                    g_status.failed = 1;
                    g_status.error_code = sys_status.error_info.error_code ? sys_status.error_info.error_code : -1;
                    strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
                    if (installer_is_nospace_error(sys_status.error_info.error_code)) {
                        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                 "Not enough free space for installation (0x%08X)",
                                 (unsigned)sys_status.error_info.error_code);
                    } else if (sname) {
                        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                 "System install error: %s (0x%08X)", sname, (unsigned)g_status.error_code);
                    } else {
                        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                                 "System install error: 0x%08X", (unsigned)g_status.error_code);
                    }
                    pthread_mutex_unlock(&g_installer_mutex);
                    if (installer_is_nospace_error(sys_status.error_info.error_code)) {
                        ps5_notify("Not enough free space for installation");
                    } else if (sname) {
                        ps5_notify("System install error: 0x%08X (%s)", g_status.error_code, sname);
                    } else {
                        ps5_notify("System install error: 0x%08X", g_status.error_code);
                    }
                    break;
                }

                if (is_filesystem_install) {
                    if (g_status.total_bytes == 0 && sys_status.total_size > 0) {
                        pthread_mutex_lock(&g_installer_mutex);
                        g_status.total_bytes = sys_status.total_size;
                        pthread_mutex_unlock(&g_installer_mutex);
                        total = sys_status.total_size;
                    }
                    /* In direct storage mode, no progress tracking is displayed. */
                } else {
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

                if (!is_filesystem_install) {
                    /* Stream install: check if HTTP server finished serving all bytes */
                    if (!stream_done) {
                        if (total > 0 && down >= total) {
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
                    }

                    is_sys_done = (strcmp(sys_status.status, "playable") == 0 || strcmp(sys_status.status, "completed") == 0);
                    if (is_sys_done && stream_done) {
                        install_log("[INSTALLER] System install completed successfully (status='%s')", sys_status.status);
                        pthread_mutex_lock(&g_installer_mutex);
                        installer_complete_locked();
                        pthread_mutex_unlock(&g_installer_mutex);
                        ps5_notify("%s is ready to play!", g_status.title_name[0] ? g_status.title_name : clean_pkg_name);
                        break;
                    }
                }
            }
        }

        /* Check if 100% of bytes have been delivered */
        if (!is_filesystem_install && !stream_done && total > 0 && down >= total) {
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
        if (!is_filesystem_install && stream_done) {
            if (time(NULL) - stream_done_time >= 3 && title_id_copy[0] != '\0') {
                int verified = check_package_verified_installed(title_id_copy, kind_copy, content_copy, expect_ver);
                if (verified) {
                    pthread_mutex_lock(&g_installer_mutex);
                    installer_complete_locked();
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
            if (is_filesystem_install || down != last_log_bytes || waiting_disc) {
                install_log("[%s] %llu / %llu bytes (%.1f%%), status='%s', waiting_disc=%d",
                            is_filesystem_install ? "INSTALL" : "STREAM",
                            (unsigned long long)down, (unsigned long long)total,
                            total > 0 ? ((float)down / (float)total) * 100.0f : 0.0f,
                            status_copy, waiting_disc);
                last_log_bytes = down;
            }
            last_log_time = now;
        }

        sleep(1);
    }
    install_service_close(&service);

#else
    /* Mock streaming simulation for host tests */
    if (is_filesystem_install) {
        usleep(50000);
        installer_submitted();
    } else if (getenv("PKG_TEST_SIMULATE_0x80B21121") != NULL && can_fallback_to_direct) {
        install_log("[INSTALLER] Helper pid=999 install returned 0x80B21121 (SCE_PLAYGO_ERROR_CORE_NET_NOT_CONNECTED)");
        install_log("[INSTALLER] Network not connected (0x80B21121); falling back to direct storage install for %s", worker_pkg_path);
        installer_set_network_offline_cached(1);
        stream_server_session_stop();
        is_filesystem_install = 1;
        can_fallback_to_direct = 0;
        snprintf(target_uri, sizeof(target_uri), "%s", worker_pkg_path);
        icon_uri[0] = '\0';

        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_direct_storage = 1;
        g_status.downloaded_bytes = 0;
        g_status.progress_percent = -1.0f;
        if (g_status.total_bytes == 0) {
            struct stat pkg_st;
            if (stat(worker_pkg_path, &pkg_st) == 0 && pkg_st.st_size > 0) {
                g_status.total_bytes = (uint64_t)pkg_st.st_size;
            }
        }
        strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing via direct storage. Track progress in the PS5 home menu. (Tip: connect to any network to see detailed real-time progress here).");
        pthread_mutex_unlock(&g_installer_mutex);
        ps5_notify("Direct storage install started. Track progress in PS5 menu.");

        usleep(50000);

        installer_submitted();
    } else if (getenv("PKG_TEST_SIMULATE_0x80B21104") != NULL) {
        int err = (int)0x80B21104u;
        install_log("[INSTALLER] Helper pid=999 install returned 0x80B21104 (SCE_PLAYGO_ERROR_CORE_NO_FREE_SPACE)");
        pthread_mutex_lock(&g_installer_mutex);
        g_status.is_installing = 0;
        g_status.failed = 1;
        g_status.error_code = err;
        strncpy(g_status.status_str, "error", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Not enough free space for installation (0x%08X)", (unsigned)err);
        pthread_mutex_unlock(&g_installer_mutex);
        ps5_notify("Not enough free space for installation");
        if (!is_filesystem_install) {
            stream_server_session_stop();
        }
        free(extracted_icon);
        return NULL;
    } else {
        char mock_pkg_path[512];
        pthread_mutex_lock(&g_installer_mutex);
        strncpy(mock_pkg_path, g_status.pkg_path, sizeof(mock_pkg_path) - 1);
        mock_pkg_path[sizeof(mock_pkg_path) - 1] = '\0';
        pthread_mutex_unlock(&g_installer_mutex);
        int mock_read_failed = 0;
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
                if (n <= 0) {
                    mock_read_failed = 1;
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
        else mock_read_failed = 1;
        if (vs_sim) free(vs_sim);

        pthread_mutex_lock(&g_installer_mutex);
        if (mock_read_failed && !g_cancel_stream && g_status.is_installing) {
            g_status.is_installing = 0;
            g_status.failed = 1;
            snprintf(g_status.status_str, sizeof(g_status.status_str), "error");
            snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Package source stopped providing data");
        }
        if (!g_cancel_stream && !g_status.failed && g_status.is_installing) {
            installer_complete_locked();
        }
        pthread_mutex_unlock(&g_installer_mutex);
    }
#endif

    /* The scheduler waits for the wrapper to report idle AFTER cleanup. */
    ws_live_abort();
    if (!is_filesystem_install) stream_server_session_stop();
    ws_live_destroy();
    free(extracted_icon);
    return NULL;
}

static void *stream_worker_entry(void *arg) {
    void *result = stream_installer_worker(arg);
    pthread_mutex_lock(&g_installer_mutex);
    g_worker_busy = 0;
    pthread_mutex_unlock(&g_installer_mutex);
    return result;
}

int installer_is_busy(void) {
    pthread_mutex_lock(&g_installer_mutex);
    int busy = g_worker_busy || g_cancel_cleanup || g_status.is_installing;
    pthread_mutex_unlock(&g_installer_mutex);
    return busy;
}

int installer_init(const char *server_url) {
    pthread_mutex_lock(&g_installer_mutex);
    memset(&g_status, 0, sizeof(g_status));
    strncpy(g_status.status_str, "idle", sizeof(g_status.status_str) - 1);
    g_status.last_poll_time = time(NULL);
    g_cancel_stream = 0;
    g_worker_busy = g_cancel_cleanup = 0;

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

    /* Catalog DLC queries and leftover removal still use the parent's
     * AppInstUtil client. Package submission/status and shortcut registration
     * use separate processes and never share this session. */
#if defined(__Prospero__) || defined(PS5_BUILD)
    extern int sceAppInstUtilInitialize(void);
    int ret = sceAppInstUtilInitialize();
    if (ret != 0) {
        printf("[PKG Manager] sceAppInstUtilInitialize returned 0x%08X\n", ret);
        ps5_notify("Internal PKG Manager: app install service returned 0x%08X", ret);
    }
#endif

    g_monitor_running = 1;
    if (pthread_create(&g_monitor_thread, NULL, installer_monitor_worker, NULL) != 0) {
        g_monitor_running = 0;
        g_monitor_thread_created = 0;
        return -1;
    }
    g_monitor_thread_created = 1;
    if (install_queue_init() != 0) {
        g_monitor_running = 0;
        pthread_join(g_monitor_thread, NULL);
        g_monitor_thread_created = 0;
        return -1;
    }
    return 0;
}

static int installer_start_internal(const char *pkg_path) {
    if (!pkg_path || pkg_path[0] == '\0') {
        return -1;
    }

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
        g_stream_thread_created = 0;
        have_old = 1;
    }
    pthread_mutex_unlock(&g_installer_mutex);
    if (have_old) {
        pthread_join(old_thr, NULL);
    }

    char pkg_path_copy[512];
    strncpy(pkg_path_copy, pkg_path, sizeof(pkg_path_copy) - 1);
    pkg_path_copy[sizeof(pkg_path_copy) - 1] = '\0';

    /* Verify that the package file exists and is accessible before proceeding */
    if (virtual_stream_check_path(pkg_path_copy) != 0) {
        install_log("[INSTALLER] Package file does not exist or cannot be opened: %s", pkg_path_copy);
        ps5_notify("Package file not found!");
        return -4; /* File not found */
    }

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
            split_pkg_t fb_set;
            if (split_pkg_detect(pkg_path_copy, &fb_set)) {
                detail.file_size = fb_set.total;
                detail.total_pkg_size = fb_set.total;
                {
                    /* Diagnostics: why could the header not be parsed? */
                    unsigned char hb[64];
                    ssize_t hn = split_pkg_pread(&fb_set, hb, sizeof(hb), 0);
                    char hex[64 * 2 + 1];
                    size_t hx = 0;
                    for (ssize_t i = 0; i < hn && hx + 2 < sizeof(hex); i++)
                        hx += (size_t)snprintf(hex + hx, sizeof(hex) - hx, "%02X", hb[i]);
                    install_log("[INSTALLER] WARNING: piece set: %u pieces total=%llu; header unparsable, first %d bytes: %s",
                                (unsigned)fb_set.count, (unsigned long long)fb_set.total, (int)hn, hex);
                    for (uint32_t i = 0; i < fb_set.count; i++)
                        install_log("[INSTALLER] WARNING: piece %u size=%llu", (unsigned)i, (unsigned long long)fb_set.size[i]);
                }
            }
        }
    }

    /* Multi-part packages are only supported on local drives (USB / optical discs) */
    if ((detail.is_multipart || strstr(pkg_path_copy, ".part") != NULL || strstr(pkg_path_copy, ".pkg.part") != NULL) &&
        strncmp(pkg_path_copy, "smb://", 6) == 0) {
        ps5_notify("Multi-part packages are only supported on USB/Disc!");
        return -13;
    }

    /* Validate against every storage destination supported by this package's
     * platform. PS4 can use USB (/mnt/ext0); PS5 cannot. */
    uint64_t required_space = detail.total_pkg_size > 0 ? detail.total_pkg_size : detail.file_size;
    int storage_check = validate_install_storage(&detail, required_space);
    if (storage_check != 0) return storage_check;

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
    int is_disc_start = (strstr(pkg_path_copy, "/mnt/disc") != NULL ||
                         strstr(pkg_path_copy, "/disc") != NULL ||
                         strstr(pkg_path_copy, "_disc") != NULL);
    int is_offline_direct = is_usb_or_disc_path(pkg_path_copy) && !detail.is_multipart && !installer_is_network_connected();
    if (is_offline_direct) {
        g_status.is_direct_storage = 1;
        g_status.progress_percent = -1.0f;
        strncpy(g_status.status_str, "installing", sizeof(g_status.status_str) - 1);
        snprintf(g_status.prompt_message, sizeof(g_status.prompt_message),
                 "Installing via direct storage. Track progress in the PS5 home menu. (Tip: connect to any network to see detailed real-time progress here).");
    } else {
        g_status.is_direct_storage = 0;
        strncpy(g_status.status_str, "transferring", sizeof(g_status.status_str) - 1);
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
        g_status.progress_percent = 0.0f;
    }
    g_status.status_str[sizeof(g_status.status_str) - 1] = '\0';
    g_status.total_bytes = detail.total_pkg_size > 0 ? detail.total_pkg_size : detail.file_size;
    g_status.downloaded_bytes = 0;
    g_status.stream_served_bytes = 0;
    g_status.start_time = time(NULL);
    g_status.last_poll_time = time(NULL);
    g_cancel_stream = 0;

    /* Every install — single or multi-part — streams through the background
       worker so progress and completion are tracked uniformly.
       (Previous worker already joined above, without holding the lock.) */

    /* Launch background stream installer pipeline */
    g_worker_busy = 1;
    if (pthread_create(&g_stream_thread, NULL, stream_worker_entry, NULL) != 0) {
        g_worker_busy = 0;
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

int installer_start(const char *pkg_path) {
    pthread_mutex_lock(&g_start_mutex);
    int rc = installer_start_internal(pkg_path);
    pthread_mutex_unlock(&g_start_mutex);
    return rc;
}

int installer_start_batch(const char *base_pkg_path, const char *update_pkg_path) {
    if (!base_pkg_path || base_pkg_path[0] == '\0' ||
        !update_pkg_path || update_pkg_path[0] == '\0') {
        return -1;
    }
    return install_queue_add_paths(base_pkg_path, update_pkg_path);
}

/* NEW: start an install from a live RAM session ("live:<id>", Direct
 * Install without any disk spool). Mirrors installer_start's checks and
 * commits, but metadata comes from pkg_parser_parse_mem over the uploaded
 * header cache and total size comes from the browser. Multi-part is
 * refused (live pushes are single packages). The existing background
 * worker runs unchanged: "live:<id>" flows through to
 * stream_server_session_start -> virtual_stream_open's live: scheme. */
static int installer_start_live_internal(const char *live_uri) {
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

    /* The browser can read param.json/SFO at arbitrary package offsets
     * before streaming. The one-segment live header cache often cannot. */
    char browser_title[256] = {0}, browser_id[64] = {0};
    char browser_version[32] = {0}, browser_kind[16] = {0};
    if (ws_direct_get_metadata(sid, browser_title, sizeof(browser_title),
                               browser_id, sizeof(browser_id), browser_version,
                               sizeof(browser_version), browser_kind,
                               sizeof(browser_kind)) == 0) {
        if (browser_title[0]) snprintf(detail.title_name, sizeof(detail.title_name), "%s", browser_title);
        if (browser_id[0]) snprintf(detail.title_id, sizeof(detail.title_id), "%s", browser_id);
        if (browser_version[0]) snprintf(detail.app_version, sizeof(detail.app_version), "%s", browser_version);
        if (!strcmp(browser_kind, "base") || !strcmp(browser_kind, "update") || !strcmp(browser_kind, "dlc"))
            snprintf(detail.pkg_type_str, sizeof(detail.pkg_type_str), "%s", browser_kind);
    }

    if (detail.is_multipart) {
        ps5_notify("Live install supports single packages only");
        return -13;
    }

    char live_path_copy[512];
    strncpy(live_path_copy, live_uri, sizeof(live_path_copy) - 1);
    live_path_copy[sizeof(live_path_copy) - 1] = '\0';

    /* 1x space: installed output only, no spool copy. USB is eligible for
     * PS4 live installs too, while PS5 remains limited to internal/M.2. */
    uint64_t required_space = detail.total_pkg_size > 0 ? detail.total_pkg_size : live_total;
    int storage_check = validate_install_storage(&detail, required_space);
    if (storage_check != 0) return storage_check;

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

    g_worker_busy = 1;
    if (pthread_create(&g_stream_thread, NULL, stream_worker_entry, NULL) != 0) {
        g_worker_busy = 0;
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

int installer_start_live(const char *live_uri) {
    pthread_mutex_lock(&g_start_mutex);
    pthread_mutex_lock(&g_installer_mutex);
    int busy = g_worker_busy || g_cancel_cleanup || g_status.is_installing;
    int reap = !busy && g_stream_thread_created;
    pthread_t previous = g_stream_thread;
    if (reap) g_stream_thread_created = 0;
    pthread_mutex_unlock(&g_installer_mutex);
    if (reap) pthread_join(previous, NULL);
    int rc = busy ? -2 : installer_start_live_internal(live_uri);
    pthread_mutex_unlock(&g_start_mutex);
    return rc;
}

int installer_cancel_path(const char *expected_path) {
    pthread_mutex_lock(&g_installer_mutex);
    if (!g_status.is_installing || (expected_path && strcmp(g_status.pkg_path, expected_path))) {
        pthread_mutex_unlock(&g_installer_mutex);
        return -1;
    }
    if (g_status.is_direct_storage) {
        install_log("[INSTALLER] Cancel rejected: direct storage install cannot be canceled (managed by PS5 system)");
        pthread_mutex_unlock(&g_installer_mutex);
        return -2;
    }
    install_log("[INSTALLER] Cancel requested");
    g_cancel_stream = 1;
    g_cancel_cleanup = 1;
    g_status.is_installing = 0;
    g_status.failed = 1;
    strncpy(g_status.status_str, "canceled", sizeof(g_status.status_str) - 1);
    snprintf(g_status.prompt_message, sizeof(g_status.prompt_message), "Installation was canceled");
    pthread_mutex_unlock(&g_installer_mutex);
    /* NEW: unblock live readers before the stop drains vs_refs, then free. */
    ws_live_abort();
    stream_server_session_stop_keep_log();
    ws_live_destroy();
    pthread_mutex_lock(&g_installer_mutex);
    g_cancel_cleanup = 0;
    pthread_mutex_unlock(&g_installer_mutex);
    ps5_notify("Installation canceled");
    return 0;
}

int installer_cancel(void) {
    return installer_cancel_path(NULL);
}

int installer_detach_direct_storage(int *out_update_skipped) {
    if (out_update_skipped) *out_update_skipped = 0;
    /* Hiding a panel never changes installation or queue ownership. */
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
    char esc_pkg_kind[64];
    char esc_status[64];
    char esc_prompt[512];

    escape_json_str(g_status.pkg_path, esc_path, sizeof(esc_path));
    escape_json_str(g_status.title_id, esc_title_id, sizeof(esc_title_id));
    escape_json_str(g_status.title_name, esc_title_name, sizeof(esc_title_name));
    escape_json_str(g_status.content_id, esc_content_id, sizeof(esc_content_id));
    escape_json_str(g_status.pkg_kind, esc_pkg_kind, sizeof(esc_pkg_kind));
    escape_json_str(g_status.status_str, esc_status, sizeof(esc_status));
    escape_json_str(g_status.prompt_message, esc_prompt, sizeof(esc_prompt));

    snprintf(json, 4096,
        "{"
        "\"is_installing\":%s,"
        "\"pkg_path\":\"%s\","
        "\"title_id\":\"%s\","
        "\"title_name\":\"%s\","
        "\"content_id\":\"%s\","
        "\"pkg_kind\":\"%s\","
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
        "\"is_direct_storage\":%s,"
        "\"prompt_message\":\"%s\""
        "}",
        g_status.is_installing ? "true" : "false",
        esc_path,
        esc_title_id,
        esc_title_name,
        esc_content_id,
        esc_pkg_kind,
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
        g_status.is_direct_storage ? "true" : "false",
        esc_prompt
    );

    pthread_mutex_unlock(&g_installer_mutex);
    return json;
}

void installer_shutdown(void) {
    install_queue_shutdown();
    g_cancel_stream = 1;
    g_monitor_running = 0;
    pthread_mutex_lock(&g_installer_mutex);
    pthread_mutex_unlock(&g_installer_mutex);
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
    s_cached_offline = 0;
    s_cached_offline_time = 0;
#if defined(__Prospero__) || defined(PS5_BUILD)
    extern int sceAppInstUtilTerminate(void);
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

static int system_get_external_storage_info(const char *env_name,
                                            const char *default_path,
                                            const char *force_fail_env,
                                            uint64_t *out_free,
                                            uint64_t *out_total,
                                            uint64_t *out_used) {
    if (!out_free || !out_total || !out_used) return -1;
    *out_free = 0;
    *out_total = 0;
    *out_used = 0;

    const char *env_path = getenv(env_name);
    const char *ext_path = (env_path && env_path[0] != '\0') ? env_path : default_path;

    if (force_fail_env && getenv(force_fail_env)) {
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

int system_get_nvme_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used) {
    return system_get_external_storage_info("PKG_EXT1_DIR", "/mnt/ext1",
                                            "PKG_FORCE_NVME_SPACE_FAIL",
                                            out_free, out_total, out_used);
}

int system_get_usb_storage_info(uint64_t *out_free, uint64_t *out_total, uint64_t *out_used) {
    return system_get_external_storage_info("PKG_EXT0_DIR", "/mnt/ext0",
                                            "PKG_FORCE_USB_SPACE_FAIL",
                                            out_free, out_total, out_used);
}
