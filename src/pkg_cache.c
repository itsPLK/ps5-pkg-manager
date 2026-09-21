/*
 * PKG Manager - Package Metadata Cache & Application Settings
 *
 * Manages persisted settings and package caching on local drives
 * and network shares.
 */

#include "pkg_cache.h"
#include "pkg_parser.h"
#include "smb_client.h"
#include "icon_blurhash.h"
#include "miniz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

static app_settings_t g_settings = {
    .move_installed_to_end = 1,
    .fade_installed_packages = 1,
    .all_sources_mode = 0,
    .pkg_install_debug = 0,
    .smb_share_count = 0
};

static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *get_settings_file_path(void) {
    const char *env_path = getenv("PKG_SETTINGS_PATH");
    if (env_path && env_path[0] != '\0') {
        return env_path;
    }
    struct stat st;
    if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir("/data/pkgmgr", 0777);
        return "/data/pkgmgr/settings.json";
    }
    return "/tmp/pkgmgr_settings.json";
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    strncpy(tmp, path, sizeof(tmp));

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0777) != 0) return -1;
            }
            tmp[i] = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        return mkdir(tmp, 0777);
    }
    return 0;
}

const char *pkg_cache_get_dir(void) {
    const char *env_path = getenv("PKG_CACHE_DIR");
    if (env_path && env_path[0] != '\0') {
        mkdir_p(env_path);
        return env_path;
    }
    struct stat st;
    if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode)) {
        mkdir_p("/data/pkgmgr/cache");
        return "/data/pkgmgr/cache";
    }
    mkdir_p("/tmp/pkgmgr/cache");
    return "/tmp/pkgmgr/cache";
}

static void load_settings_from_disk(void) {
    const char *path = get_settings_file_path();
    FILE *f = fopen(path, "r");
    if (!f) return;

    char *buf = (char *)malloc(16384);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t rd = fread(buf, 1, 16383, f);
    fclose(f);
    if (rd == 0) {
        free(buf);
        return;
    }
    buf[rd] = '\0';

    char *move_ptr = strstr(buf, "\"move_installed_to_end\":");
    if (move_ptr) {
        if (strncmp(move_ptr + 24, "false", 5) == 0 || strncmp(move_ptr + 25, "false", 5) == 0) {
            g_settings.move_installed_to_end = 0;
        } else {
            g_settings.move_installed_to_end = 1;
        }
    }

    char *fade_ptr = strstr(buf, "\"fade_installed_packages\":");
    if (fade_ptr) {
        if (strncmp(fade_ptr + 26, "false", 5) == 0 || strncmp(fade_ptr + 27, "false", 5) == 0) {
            g_settings.fade_installed_packages = 0;
        } else {
            g_settings.fade_installed_packages = 1;
        }
    }

    char *all_src_ptr = strstr(buf, "\"all_sources_mode\":");
    if (all_src_ptr) {
        if (strncmp(all_src_ptr + 19, "true", 4) == 0 || strncmp(all_src_ptr + 20, "true", 4) == 0) {
            g_settings.all_sources_mode = 1;
        } else {
            g_settings.all_sources_mode = 0;
        }
    }

    char *debug_ptr = strstr(buf, "\"pkg_install_debug\":");
    if (debug_ptr) {
        if (strncmp(debug_ptr + 20, "true", 4) == 0 || strncmp(debug_ptr + 21, "true", 4) == 0) {
            g_settings.pkg_install_debug = 1;
        } else {
            g_settings.pkg_install_debug = 0;
        }
    }

    pkg_cache_parse_smb_shares(buf, g_settings.smb_shares, &g_settings.smb_share_count);
    free(buf);
}

static void escape_json_str(const char *src, char *dst, size_t dst_max);

static void save_settings_to_disk(void) {
    const char *path = get_settings_file_path();
    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return;

    fprintf(f, "{\n"
               "  \"move_installed_to_end\": %s,\n"
               "  \"fade_installed_packages\": %s,\n"
               "  \"all_sources_mode\": %s,\n"
               "  \"pkg_install_debug\": %s,\n"
               "  \"smb_shares\": [\n",
            g_settings.move_installed_to_end ? "true" : "false",
            g_settings.fade_installed_packages ? "true" : "false",
            g_settings.all_sources_mode ? "true" : "false",
            g_settings.pkg_install_debug ? "true" : "false");

    for (int i = 0; i < g_settings.smb_share_count; i++) {
        const smb_share_config_t *s = &g_settings.smb_shares[i];
        char esc_id[128], esc_label[256], esc_server[256], esc_share[256], esc_path[512];
        char esc_user[256], esc_pass[256], esc_workgroup[256];
        escape_json_str(s->id, esc_id, sizeof(esc_id));
        escape_json_str(s->label, esc_label, sizeof(esc_label));
        escape_json_str(s->server, esc_server, sizeof(esc_server));
        escape_json_str(s->share, esc_share, sizeof(esc_share));
        escape_json_str(s->path, esc_path, sizeof(esc_path));
        escape_json_str(s->username, esc_user, sizeof(esc_user));
        escape_json_str(s->password, esc_pass, sizeof(esc_pass));
        escape_json_str(s->workgroup, esc_workgroup, sizeof(esc_workgroup));

        fprintf(f, "    {\n"
                   "      \"id\": \"%s\",\n"
                   "      \"label\": \"%s\",\n"
                   "      \"server\": \"%s\",\n"
                   "      \"port\": %d,\n"
                   "      \"share\": \"%s\",\n"
                   "      \"path\": \"%s\",\n"
                   "      \"username\": \"%s\",\n"
                   "      \"password\": \"%s\",\n"
                   "      \"workgroup\": \"%s\",\n"
                   "      \"is_read_only\": %s,\n"
                   "      \"enabled\": %s\n"
                   "    }%s\n",
                esc_id, esc_label, esc_server, s->port, esc_share, esc_path,
                esc_user, esc_pass, esc_workgroup,
                s->is_read_only ? "true" : "false",
                s->enabled ? "true" : "false",
                (i + 1 < g_settings.smb_share_count) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp_path);
        return;
    }
    fclose(f);
    rename(tmp_path, path);
}

void pkg_cache_init(void) {
    pthread_mutex_lock(&g_cache_mutex);
    load_settings_from_disk();
    pthread_mutex_unlock(&g_cache_mutex);
}

void pkg_cache_get_settings(app_settings_t *out_settings) {
    if (!out_settings) return;
    pthread_mutex_lock(&g_cache_mutex);
    memcpy(out_settings, &g_settings, sizeof(app_settings_t));
    pthread_mutex_unlock(&g_cache_mutex);
}

int pkg_cache_set_settings(const app_settings_t *settings) {
    if (!settings) return -1;
    pthread_mutex_lock(&g_cache_mutex);
    g_settings.move_installed_to_end = settings->move_installed_to_end ? 1 : 0;
    g_settings.fade_installed_packages = settings->fade_installed_packages ? 1 : 0;
    g_settings.all_sources_mode = settings->all_sources_mode ? 1 : 0;
    g_settings.pkg_install_debug = settings->pkg_install_debug ? 1 : 0;
    g_settings.smb_share_count = (settings->smb_share_count <= MAX_SMB_SHARES) ? settings->smb_share_count : MAX_SMB_SHARES;
    if (g_settings.smb_share_count < 0) g_settings.smb_share_count = 0;
    for (int i = 0; i < g_settings.smb_share_count; i++) {
        memcpy(&g_settings.smb_shares[i], &settings->smb_shares[i], sizeof(smb_share_config_t));
    }
    save_settings_to_disk();
    pthread_mutex_unlock(&g_cache_mutex);
    return 0;
}

int pkg_cache_calc_checksum(const char *pkg_path, char *out_checksum, size_t out_max) {
    if (!pkg_path || !out_checksum || out_max < 33) return -1;

    if (strncmp(pkg_path, "smb://", 6) == 0) {
        return smb_client_calc_checksum(pkg_path, out_checksum, out_max);
    }

    struct stat st;
    if (stat(pkg_path, &st) != 0) return -1;

    FILE *f = fopen(pkg_path, "rb");
    if (!f) return -1;

    uint8_t hdr[4096];
    size_t rd = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);

    uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                      (const unsigned char *)PKG_CACHE_FORMAT_TAG,
                                      sizeof(PKG_CACHE_FORMAT_TAG) - 1);
    if (rd > 0) {
        crc = (uint32_t)mz_crc32(crc, hdr, rd);
    }

    /* Checksum: 32 hex characters combining mtime (8 hex), file_size (16 hex), header CRC (8 hex) */
    snprintf(out_checksum, out_max, "%08x%016llx%08x",
             (uint32_t)st.st_mtime,
             (unsigned long long)st.st_size,
             (uint32_t)crc);
    return 0;
}

static void escape_json_str(const char *src, char *dst, size_t dst_max) {
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 2 < dst_max; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '"' || c == '\\') {
            dst[d++] = '\\';
            dst[d++] = (char)c;
        } else if (c == '\n') {
            dst[d++] = '\\';
            dst[d++] = 'n';
        } else if (c == '\r') {
            dst[d++] = '\\';
            dst[d++] = 'r';
        } else if (c == '\t') {
            dst[d++] = '\\';
            dst[d++] = 't';
        } else if (c >= 32) {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

static void extract_json_field(const char *json, const char *key, char *out_val, size_t out_max) {
    out_val[0] = '\0';
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "\"%s\":", key);
    char *p = strstr(json, search_key);
    if (!p) return;

    p += strlen(search_key);
    while (*p == ' ' || *p == '\t') p++;

    if (*p == '"') {
        p++;
        size_t d = 0;
        while (*p != '\0' && *p != '"' && d + 1 < out_max) {
            if (*p == '\\' && *(p + 1) != '\0') {
                p++;
            }
            out_val[d++] = *p++;
        }
        out_val[d] = '\0';
    } else {
        size_t d = 0;
        while (*p != '\0' && *p != ',' && *p != '}' && *p != '\n' && *p != '\r' && *p != ' ' && d + 1 < out_max) {
            out_val[d++] = *p++;
        }
        out_val[d] = '\0';
    }
}

void pkg_cache_parse_smb_shares(const char *buf, smb_share_config_t *shares, int *count) {
    if (!buf || !shares || !count) return;
    *count = 0;

    const char *arr = strstr(buf, "\"smb_shares\"");
    if (!arr) return;
    arr = strchr(arr, '[');
    if (!arr) return;
    arr++;

    while (*arr != '\0' && *arr != ']') {
        const char *obj_start = strchr(arr, '{');
        if (!obj_start) break;
        /* Find matching '}' respecting quoted strings so passwords
         * containing '}' don't truncate the object. */
        const char *q = obj_start;
        int in_str = 0, esc = 0;
        const char *obj_end = NULL;
        for (q = obj_start; *q; q++) {
            if (esc) { esc = 0; continue; }
            if (*q == '\\' && in_str) { esc = 1; continue; }
            if (*q == '"') { in_str = !in_str; continue; }
            if (!in_str && *q == '}') { obj_end = q; break; }
            if (!in_str && *q == ']') break;
        }
        if (!obj_end) break;

        size_t len = (size_t)(obj_end - obj_start + 1);
        char *item = (char *)malloc(len + 1);
        if (!item) break;
        strncpy(item, obj_start, len);
        item[len] = '\0';

        if (*count < MAX_SMB_SHARES) {
            smb_share_config_t *s = &shares[*count];
            memset(s, 0, sizeof(*s));
            s->enabled = 1;
            s->port = SMB_DEFAULT_PORT;

            extract_json_field(item, "id", s->id, sizeof(s->id));
            extract_json_field(item, "label", s->label, sizeof(s->label));
            extract_json_field(item, "server", s->server, sizeof(s->server));
            extract_json_field(item, "share", s->share, sizeof(s->share));
            extract_json_field(item, "path", s->path, sizeof(s->path));
            extract_json_field(item, "username", s->username, sizeof(s->username));
            extract_json_field(item, "password", s->password, sizeof(s->password));
            extract_json_field(item, "workgroup", s->workgroup, sizeof(s->workgroup));

            char val[64];
            extract_json_field(item, "port", val, sizeof(val));
            if (val[0] != '\0') {
                int p = atoi(val);
                if (p > 0) s->port = p;
            }

            extract_json_field(item, "is_read_only", val, sizeof(val));
            s->is_read_only = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

            extract_json_field(item, "enabled", val, sizeof(val));
            if (val[0] != '\0') {
                s->enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
            }

            if (s->server[0] != '\0' && (s->share[0] != '\0' || strchr(s->server, '/') != NULL || strchr(s->server, '\\') != NULL)) {
                if (s->id[0] == '\0') {
                    snprintf(s->id, sizeof(s->id), "smb%d", *count);
                }
                (*count)++;
            }
        }

        free(item);
        arr = obj_end + 1;
    }
}

int pkg_cache_lookup(const char *checksum, pkg_detail_t *out_detail) {
    if (!checksum || !out_detail) return -1;

    char meta_path[1024];
    snprintf(meta_path, sizeof(meta_path), "%s/%s/meta.json", pkg_cache_get_dir(), checksum);

    FILE *f = fopen(meta_path, "r");
    if (!f) return -1;

    char buf[8192];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    /* If meta doesn't fit, treat as corrupt (avoid truncated parse). */
    if (rd == sizeof(buf) - 1 && !feof(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (rd == 0) return -1;
    buf[rd] = '\0';

    memset(out_detail, 0, sizeof(*out_detail));

    char val[512];
    extract_json_field(buf, "title_id", out_detail->title_id, sizeof(out_detail->title_id));
    extract_json_field(buf, "title_name", out_detail->title_name, sizeof(out_detail->title_name));
    extract_json_field(buf, "localized_titles", out_detail->localized_titles, sizeof(out_detail->localized_titles));
    extract_json_field(buf, "default_language", out_detail->default_language, sizeof(out_detail->default_language));
    extract_json_field(buf, "content_id", out_detail->content_id, sizeof(out_detail->content_id));
    extract_json_field(buf, "app_version", out_detail->app_version, sizeof(out_detail->app_version));
    extract_json_field(buf, "pkg_type_str", out_detail->pkg_type_str, sizeof(out_detail->pkg_type_str));
    extract_json_field(buf, "category", out_detail->category, sizeof(out_detail->category));

    extract_json_field(buf, "file_size", val, sizeof(val));
    if (val[0]) out_detail->file_size = (uint64_t)strtoull(val, NULL, 10);

    extract_json_field(buf, "total_pkg_size", val, sizeof(val));
    if (val[0]) out_detail->total_pkg_size = (uint64_t)strtoull(val, NULL, 10);
    else out_detail->total_pkg_size = out_detail->file_size;

    extract_json_field(buf, "icon_offset", val, sizeof(val));
    if (val[0]) out_detail->icon_offset = (uint64_t)strtoull(val, NULL, 10);

    extract_json_field(buf, "icon_size", val, sizeof(val));
    if (val[0]) out_detail->icon_size = (uint32_t)strtoul(val, NULL, 10);

    extract_json_field(buf, "has_icon", val, sizeof(val));
    out_detail->has_icon = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

    extract_json_field(buf, "is_multipart", val, sizeof(val));
    out_detail->is_multipart = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

    extract_json_field(buf, "part_index", val, sizeof(val));
    if (val[0]) out_detail->part_index = (uint32_t)strtoul(val, NULL, 10);

    extract_json_field(buf, "total_parts", val, sizeof(val));
    if (val[0]) out_detail->total_parts = (uint32_t)strtoul(val, NULL, 10);

    extract_json_field(buf, "pkg_type", val, sizeof(val));
    if (val[0]) out_detail->pkg_type = (pkg_type_t)atoi(val);

    extract_json_field(buf, "mtime", val, sizeof(val));
    if (val[0]) out_detail->mtime = (uint64_t)strtoull(val, NULL, 10);

    extract_json_field(buf, "blurhash", out_detail->blurhash, sizeof(out_detail->blurhash));

    /* Verify if cached icon file exists */
    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/%s/icon.png", pkg_cache_get_dir(), checksum);
    struct stat icon_st;
    if (stat(icon_path, &icon_st) == 0 && icon_st.st_size > 0) {
        out_detail->has_icon = 1;
    }

    /* Stale or corrupted cache from previous bug (missing title ID): reject hit to force fresh parse */
    if (out_detail->title_id[0] == '\0' && strcmp(out_detail->title_name, "Unknown Package") == 0) {
        return -1;
    }

    out_detail->is_valid = 1;
    return 0;
}

static int write_meta_json(const char *dir_path, const pkg_detail_t *detail, const char *blurhash) {
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.json", dir_path);
    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s/meta.json.tmp", dir_path);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    char esc_title_id[128], esc_title_name[512], esc_content_id[256], esc_app_ver[64], esc_type_str[32], esc_cat[32];
    char esc_loc[PKG_LOCALIZED_TITLES_LEN * 2], esc_def_lang[64];
    escape_json_str(detail->title_id, esc_title_id, sizeof(esc_title_id));
    escape_json_str(detail->title_name, esc_title_name, sizeof(esc_title_name));
    escape_json_str(detail->localized_titles, esc_loc, sizeof(esc_loc));
    escape_json_str(detail->default_language, esc_def_lang, sizeof(esc_def_lang));
    escape_json_str(detail->content_id, esc_content_id, sizeof(esc_content_id));
    escape_json_str(detail->app_version, esc_app_ver, sizeof(esc_app_ver));
    escape_json_str(detail->pkg_type_str, esc_type_str, sizeof(esc_type_str));
    escape_json_str(detail->category, esc_cat, sizeof(esc_cat));

    fprintf(f, "{\n"
               "  \"title_id\": \"%s\",\n"
               "  \"title_name\": \"%s\",\n"
               "  \"localized_titles\": \"%s\",\n"
               "  \"default_language\": \"%s\",\n"
               "  \"content_id\": \"%s\",\n"
               "  \"app_version\": \"%s\",\n"
               "  \"file_size\": %llu,\n"
               "  \"total_pkg_size\": %llu,\n"
               "  \"icon_offset\": %llu,\n"
               "  \"icon_size\": %u,\n"
               "  \"has_icon\": %s,\n"
               "  \"is_multipart\": %s,\n"
               "  \"part_index\": %u,\n"
               "  \"total_parts\": %u,\n"
               "  \"pkg_type\": %d,\n"
               "  \"pkg_type_str\": \"%s\",\n"
               "  \"category\": \"%s\",\n"
               "  \"mtime\": %llu,\n"
               "  \"blurhash\": \"%s\"\n"
               "}\n",
            esc_title_id, esc_title_name, esc_loc, esc_def_lang, esc_content_id, esc_app_ver,
            (unsigned long long)detail->file_size,
            (unsigned long long)(detail->total_pkg_size > 0 ? detail->total_pkg_size : detail->file_size),
            (unsigned long long)detail->icon_offset, detail->icon_size,
            detail->has_icon ? "true" : "false",
            detail->is_multipart ? "true" : "false",
            detail->part_index, detail->total_parts,
            (int)detail->pkg_type, esc_type_str, esc_cat,
            (unsigned long long)detail->mtime,
            blurhash ? blurhash : "");
    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    fclose(f);
    if (rename(tmp_path, meta_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int pkg_cache_update_meta(const char *checksum, const pkg_detail_t *detail) {
    if (!checksum || !detail) return -1;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", pkg_cache_get_dir(), checksum);
    if (mkdir_p(dir_path) != 0) return -1;

    return write_meta_json(dir_path, detail, detail->blurhash);
}

int pkg_cache_save(const char *checksum, pkg_detail_t *detail) {
    if (!checksum || !detail) return -1;

    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", pkg_cache_get_dir(), checksum);
    if (mkdir_p(dir_path) != 0) return -1;

    char blurhash[64] = {0};
    if (detail->blurhash[0] != '\0' && strlen(detail->blurhash) >= BLURHASH_UPGRADE_MIN_LEN) {
        strncpy(blurhash, detail->blurhash, sizeof(blurhash) - 1);
    }

    /* Check if cached icon.png already exists */
    char icon_path[2048];
    snprintf(icon_path, sizeof(icon_path), "%s/icon.png", dir_path);
    struct stat ist;
    int icon_exists = (stat(icon_path, &ist) == 0 && ist.st_size > 0);

    /* Fetch icon bytes up-front only when needed (missing icon.png or missing blurhash) */
    uint8_t *icon_buf = NULL;
    size_t icon_sz = 0;
    if (detail->has_icon && (!icon_exists || blurhash[0] == '\0')) {
        if (pkg_parser_get_icon(detail->path, detail->icon_offset, detail->icon_size,
                                &icon_buf, &icon_sz) == 0 && icon_buf && icon_sz > 0) {
            if (blurhash[0] == '\0') {
                if (icon_compute_blurhash(icon_buf, icon_sz, blurhash, sizeof(blurhash)) == 0) {
                    strncpy(detail->blurhash, blurhash, sizeof(detail->blurhash) - 1);
                }
            }
        } else {
            if (icon_buf) free(icon_buf);
            icon_buf = NULL;
            icon_sz = 0;
        }
    }

    if (write_meta_json(dir_path, detail, blurhash) != 0) {
        if (icon_buf) free(icon_buf);
        return -1;
    }

    /* If icon.png was missing and we fetched icon_buf, write it atomically */
    if (icon_buf && icon_sz > 0) {
        if (!icon_exists) {
            char tmp_icon[2048];
            snprintf(tmp_icon, sizeof(tmp_icon), "%s/icon.png.tmp", dir_path);
            FILE *fi = fopen(tmp_icon, "wb");
            if (fi) {
                size_t w = fwrite(icon_buf, 1, icon_sz, fi);
                if (w == icon_sz && fflush(fi) == 0) {
                    fclose(fi);
                    rename(tmp_icon, icon_path);
                } else {
                    fclose(fi);
                    unlink(tmp_icon);
                }
            }
        }
        free(icon_buf);
    }

    return 0;
}

int pkg_cache_get_icon(const char *pkg_path, uint8_t **out_data, size_t *out_size) {
    if (!pkg_path || !out_data || !out_size) return -1;
    *out_data = NULL;
    *out_size = 0;

    char checksum[64];
    if (pkg_cache_calc_checksum(pkg_path, checksum, sizeof(checksum)) != 0) {
        return -1;
    }

    char icon_path[1024];
    snprintf(icon_path, sizeof(icon_path), "%s/%s/icon.png", pkg_cache_get_dir(), checksum);

    struct stat st;
    if (stat(icon_path, &st) != 0 || st.st_size <= 0) {
        return -1;
    }

    FILE *f = fopen(icon_path, "rb");
    if (!f) return -1;

    uint8_t *buf = (uint8_t *)malloc(st.st_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t rd = fread(buf, 1, st.st_size, f);
    fclose(f);
    if (rd != (size_t)st.st_size) {
        free(buf);
        return -1;
    }

    *out_data = buf;
    *out_size = rd;
    return 0;
}

static void calc_dir_stats(const char *dir_path, uint64_t *out_bytes, uint32_t *out_count) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char sub_path[1024];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (lstat(sub_path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            (*out_count)++;
            /* Count files inside checksum dir */
            DIR *sub_d = opendir(sub_path);
            if (sub_d) {
                struct dirent *fe;
                while ((fe = readdir(sub_d)) != NULL) {
                    if (fe->d_name[0] == '.') continue;
                    char file_path[2048];
                    snprintf(file_path, sizeof(file_path), "%s/%s", sub_path, fe->d_name);
                    struct stat fst;
                    if (lstat(file_path, &fst) == 0 && !S_ISLNK(fst.st_mode) && S_ISREG(fst.st_mode)) {
                        *out_bytes += (uint64_t)fst.st_size;
                    }
                }
                closedir(sub_d);
            }
        }
    }
    closedir(d);
}

char *pkg_cache_get_stats_json(void) {
    uint64_t total_bytes = 0;
    uint32_t total_count = 0;

    const char *cache_dir = pkg_cache_get_dir();
    calc_dir_stats(cache_dir, &total_bytes, &total_count);

    char *json = (char *)malloc(1024);
    if (!json) return NULL;

    char esc_dir[512];
    escape_json_str(cache_dir, esc_dir, sizeof(esc_dir));
    snprintf(json, 1024,
        "{\"total_bytes\":%llu,\"total_count\":%u,\"cache_path\":\"%s\",\"drives\":[]}",
        (unsigned long long)total_bytes, total_count, esc_dir);
    return json;
}

static int64_t remove_dir_contents(const char *dir_path) {
    int64_t freed = 0;
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char sub_path[1024];
        snprintf(sub_path, sizeof(sub_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        /* Never follow symlinks: attacker-planted link in cache must not
         * cause deletion outside the cache (LAN-triggerable). */
        if (lstat(sub_path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) {
            unlink(sub_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            freed += remove_dir_contents(sub_path);
            rmdir(sub_path);
        } else if (S_ISREG(st.st_mode)) {
            freed += (int64_t)st.st_size;
            unlink(sub_path);
        }
    }
    closedir(d);
    return freed;
}

int64_t pkg_cache_clear(void) {
    const char *cache_dir = pkg_cache_get_dir();
    return remove_dir_contents(cache_dir);
}
