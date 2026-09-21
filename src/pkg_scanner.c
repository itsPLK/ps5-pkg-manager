/*
 * PKG Manager - Package and Storage Drive Scanner
 *
 * Discovers USB drives, Blu-ray discs, internal storage, and SMB shares.
 * Manages manifest caching, quick rescans, and multi-part package indexing.
 */

#include "pkg_scanner.h"
#include "pkg_cache.h"
#include "icon_blurhash.h"
#include "multipart.h"
#include "app_info.h"
#include "smb_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

static void evaluate_install_eligibility(const pkg_detail_t *pkg, int is_installed,
                                         const char *installed_version, int dlc_installed,
                                         int has_leftover, int partial,
                                         pkg_install_eligibility_t *out) {
    memset(out, 0, sizeof(*out));
    out->can_install = 1;
    out->disabled_reason = "";
    out->is_installed = is_installed;
    snprintf(out->installed_version, sizeof(out->installed_version), "%s", installed_version);

    if (pkg->is_multipart && strncmp(pkg->path, "smb://", 6) == 0) {
        out->can_install = 0;
        out->disabled_reason = "Multi-part packages are only supported on USB/Disc";
    } else if (has_leftover) {
        out->can_install = 0;
        out->disabled_reason = "Leftovers detected on console. Clean up leftovers before installing.";
    } else if (pkg->pkg_type == PKG_TYPE_BASE || pkg->pkg_type == PKG_TYPE_UNKNOWN) {
        if (out->is_installed) {
            out->can_install = 0;
            if (out->installed_version[0] && pkg->app_version[0]) {
                if (app_info_compare_versions(out->installed_version, pkg->app_version) < 0)
                    out->can_install = 1;
                else
                    out->disabled_reason = "Installed version is same or newer";
            } else {
                out->disabled_reason = "Application is already installed";
            }
        }
    } else if (pkg->pkg_type == PKG_TYPE_UPDATE || pkg->pkg_type == PKG_TYPE_DLC) {
        if (!out->is_installed) {
            out->can_install = 0;
            out->disabled_reason = partial
                ? "Base package installation was aborted. Reinstall base package first."
                : "Base package is not installed";
        } else if (pkg->pkg_type == PKG_TYPE_UPDATE && out->installed_version[0] &&
                   pkg->app_version[0] &&
                   app_info_compare_versions(out->installed_version, pkg->app_version) >= 0) {
            out->can_install = 0;
            out->disabled_reason = "Installed version is same or newer";
        } else if (pkg->pkg_type == PKG_TYPE_DLC && dlc_installed) {
            out->can_install = 0;
            out->disabled_reason = "DLC is already installed";
        }
    }
}

void pkg_scanner_check_install_eligibility(const pkg_detail_t *pkg,
                                           pkg_install_eligibility_t *out) {
    char installed_version[32] = {0};
    int is_installed = app_info_check_installed(pkg->title_id, installed_version,
                                                sizeof(installed_version));
    int dlc_installed = pkg->pkg_type == PKG_TYPE_DLC && pkg->content_id[0] &&
        app_info_check_dlc_installed(pkg->title_id, pkg->content_id);
    int has_leftover = !is_installed && app_info_check_has_leftover(pkg->title_id, NULL, 0);
    int partial = !is_installed && app_info_check_partially_installed(pkg->title_id, NULL, 0);
    evaluate_install_eligibility(pkg, is_installed, installed_version, dlc_installed,
                                 has_leftover, partial, out);
}

static int compare_pkg_by_title_name(const void *a, const void *b) {
    const pkg_detail_t *pa = (const pkg_detail_t *)a;
    const pkg_detail_t *pb = (const pkg_detail_t *)b;
    const char *na = pa->title_name[0] ? pa->title_name : "Unknown Package";
    const char *nb = pb->title_name[0] ? pb->title_name : "Unknown Package";
    int cmp = strcasecmp(na, nb);
    if (cmp != 0) return cmp;
    /* Secondary sort: Base (1) < Update (2) < DLC (3) */
    if (pa->pkg_type != pb->pkg_type) {
        return (int)pa->pkg_type - (int)pb->pkg_type;
    }
    /* Tertiary sort: version ascending */
    if (pa->app_version[0] != '\0' && pb->app_version[0] != '\0') {
        int vcmp = app_info_compare_versions(pa->app_version, pb->app_version);
        if (vcmp != 0) return vcmp;
    }
    return strcmp(pa->path, pb->path);
}

#if defined(__Prospero__) || defined(PS5_BUILD)
#include <sys/mount.h>
#endif

#define MAX_PACKAGES 4096
#define MAX_DRIVES   16
#define PKG_MANIFEST_VERSION 2

static pkg_detail_t g_packages[MAX_PACKAGES];
static size_t g_package_count = 0;

static pkg_drive_t g_drives[MAX_DRIVES];
static size_t g_drive_count = 0;

static pthread_mutex_t g_scanner_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_scan_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static pkg_scan_status_t g_scan_status = {0};
static pthread_mutex_t g_scan_status_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char path[512];
    uint64_t file_size;
    uint64_t mtime;
} scanned_file_entry_t;

static scanned_file_entry_t *g_scanned_files = NULL;
static size_t g_scanned_file_count = 0;
static size_t g_scanned_file_capacity = 0;

static int add_scanned_file_locked(const char *path, uint64_t file_size, uint64_t mtime) {
    if (!path || path[0] == '\0') return -1;
    for (size_t i = 0; i < g_scanned_file_count; i++) {
        if (strcmp(g_scanned_files[i].path, path) == 0) {
            g_scanned_files[i].file_size = file_size;
            g_scanned_files[i].mtime = mtime;
            return 0;
        }
    }
    if (g_scanned_file_count >= g_scanned_file_capacity) {
        size_t new_cap = g_scanned_file_capacity == 0 ? 128 : g_scanned_file_capacity * 2;
        scanned_file_entry_t *new_arr = (scanned_file_entry_t *)realloc(g_scanned_files, new_cap * sizeof(scanned_file_entry_t));
        if (!new_arr) return -1;
        g_scanned_files = new_arr;
        g_scanned_file_capacity = new_cap;
    }
    scanned_file_entry_t *e = &g_scanned_files[g_scanned_file_count++];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->file_size = file_size;
    e->mtime = mtime;
    return 0;
}

static int pkg_matches_drive_path(const char *pkg_path, const char *drive_path) {
    if (!pkg_path || !drive_path || drive_path[0] == '\0') return 0;
    size_t dlen = strlen(drive_path);
    if (strncmp(pkg_path, drive_path, dlen) != 0) return 0;
    if (drive_path[dlen - 1] == '/' || pkg_path[dlen] == '/' || pkg_path[dlen] == '\0') {
        return 1;
    }
    return 0;
}

static void remove_scanned_files_for_drive_locked(const char *drive_path) {
    if (!drive_path) return;
    size_t write_idx = 0;
    for (size_t i = 0; i < g_scanned_file_count; i++) {
        if (!pkg_matches_drive_path(g_scanned_files[i].path, drive_path)) {
            if (write_idx != i) {
                memcpy(&g_scanned_files[write_idx], &g_scanned_files[i], sizeof(scanned_file_entry_t));
            }
            write_idx++;
        }
    }
    g_scanned_file_count = write_idx;
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

static const char *find_matching_pair(const char *start, char open_ch, char close_ch) {
    if (!start || *start != open_ch) return NULL;
    int depth = 0, in_str = 0, esc = 0;
    for (const char *p = start; *p; p++) {
        if (esc) { esc = 0; continue; }
        if (*p == '\\' && in_str) { esc = 1; continue; }
        if (*p == '"') { in_str = !in_str; continue; }
        if (!in_str) {
            if (*p == open_ch) depth++;
            else if (*p == close_ch) {
                depth--;
                if (depth == 0) return p;
            }
        }
    }
    return NULL;
}

static int save_manifest_locked(void) {
    const char *cache_dir = pkg_cache_get_dir();
    if (!cache_dir || cache_dir[0] == '\0') return -1;

    char tmp_path[1024], manifest_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s/manifest.json.tmp", cache_dir);
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", cache_dir);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    fprintf(f, "{\n  \"version\": %d,\n  \"drives\": [\n", PKG_MANIFEST_VERSION);
    for (size_t i = 0; i < g_drive_count; i++) {
        const pkg_drive_t *d = &g_drives[i];
        char esc_id[64], esc_label[128], esc_path[512], esc_type[32];
        escape_json_str(d->id, esc_id, sizeof(esc_id));
        escape_json_str(d->label, esc_label, sizeof(esc_label));
        escape_json_str(d->path, esc_path, sizeof(esc_path));
        escape_json_str(d->type, esc_type, sizeof(esc_type));
        fprintf(f, "    {\"id\":\"%s\",\"label\":\"%s\",\"path\":\"%s\",\"type\":\"%s\",\"mounted\":%d,\"pkg_count\":%zu}%s\n",
                esc_id, esc_label, esc_path, esc_type, d->mounted, d->pkg_count,
                (i + 1 < g_drive_count) ? "," : "");
    }
    fprintf(f, "  ],\n  \"files\": [\n");
    for (size_t i = 0; i < g_scanned_file_count; i++) {
        const scanned_file_entry_t *fe = &g_scanned_files[i];
        char esc_p[1024];
        escape_json_str(fe->path, esc_p, sizeof(esc_p));
        fprintf(f, "    {\"path\":\"%s\",\"file_size\":%llu,\"mtime\":%llu}%s\n",
                esc_p, (unsigned long long)fe->file_size, (unsigned long long)fe->mtime,
                (i + 1 < g_scanned_file_count) ? "," : "");
    }
    fprintf(f, "  ],\n  \"packages\": [\n");
    for (size_t i = 0; i < g_package_count; i++) {
        const pkg_detail_t *p = &g_packages[i];
        char esc_path[1024], esc_fn[512], esc_tid[64], esc_tname[512], esc_cid[128];
        char esc_ver[64], esc_type[32], esc_cat[32], esc_bh[128];
        char esc_loc[PKG_LOCALIZED_TITLES_LEN * 2], esc_def_lang[64];
        escape_json_str(p->path, esc_path, sizeof(esc_path));
        escape_json_str(p->filename, esc_fn, sizeof(esc_fn));
        escape_json_str(p->title_id, esc_tid, sizeof(esc_tid));
        escape_json_str(p->title_name, esc_tname, sizeof(esc_tname));
        escape_json_str(p->localized_titles, esc_loc, sizeof(esc_loc));
        escape_json_str(p->default_language, esc_def_lang, sizeof(esc_def_lang));
        escape_json_str(p->content_id, esc_cid, sizeof(esc_cid));
        escape_json_str(p->app_version, esc_ver, sizeof(esc_ver));
        escape_json_str(p->pkg_type_str, esc_type, sizeof(esc_type));
        escape_json_str(p->category, esc_cat, sizeof(esc_cat));
        escape_json_str(p->blurhash, esc_bh, sizeof(esc_bh));

        fprintf(f, "    {\n"
                   "      \"path\": \"%s\",\n"
                   "      \"filename\": \"%s\",\n"
                   "      \"title_id\": \"%s\",\n"
                   "      \"title_name\": \"%s\",\n"
                   "      \"localized_titles\": \"%s\",\n"
                   "      \"default_language\": \"%s\",\n"
                   "      \"content_id\": \"%s\",\n"
                   "      \"app_version\": \"%s\",\n"
                   "      \"file_size\": %llu,\n"
                   "      \"total_pkg_size\": %llu,\n"
                   "      \"icon_offset\": %llu,\n"
                   "      \"icon_size\": %u,\n"
                   "      \"has_icon\": %s,\n"
                   "      \"is_valid\": %s,\n"
                   "      \"is_multipart\": %s,\n"
                   "      \"part_index\": %u,\n"
                   "      \"total_parts\": %u,\n"
                   "      \"pkg_type\": %d,\n"
                   "      \"pkg_type_str\": \"%s\",\n"
                   "      \"category\": \"%s\",\n"
                   "      \"mtime\": %llu,\n"
                   "      \"blurhash\": \"%s\"\n"
                   "    }%s\n",
                esc_path, esc_fn, esc_tid, esc_tname, esc_loc, esc_def_lang, esc_cid, esc_ver,
                (unsigned long long)p->file_size,
                (unsigned long long)(p->total_pkg_size > 0 ? p->total_pkg_size : p->file_size),
                (unsigned long long)p->icon_offset,
                p->icon_size,
                p->has_icon ? "true" : "false",
                p->is_valid ? "true" : "false",
                p->is_multipart ? "true" : "false",
                p->part_index, p->total_parts,
                (int)p->pkg_type, esc_type, esc_cat,
                (unsigned long long)p->mtime,
                esc_bh,
                (i + 1 < g_package_count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");

    if (fflush(f) != 0) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }
    fclose(f);
    if (rename(tmp_path, manifest_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

static int load_manifest_locked(void) {
    const char *cache_dir = pkg_cache_get_dir();
    if (!cache_dir || cache_dir[0] == '\0') return -1;

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", cache_dir);

    FILE *f = fopen(manifest_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 10 || fsize > 10 * 1024 * 1024) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[rd] = '\0';

    char manifest_version[16] = {0};
    extract_json_field(buf, "version", manifest_version, sizeof(manifest_version));
    if (atoi(manifest_version) != PKG_MANIFEST_VERSION) {
        /* Package classification rules changed; force a fresh scan rather
         * than reusing entries produced by an older parser. */
        free(buf);
        return -1;
    }

    /* Parse drives */
    g_drive_count = 0;
    const char *drives_key = strstr(buf, "\"drives\"");
    if (drives_key) {
        const char *arr_start = strchr(drives_key, '[');
        if (arr_start) {
            const char *arr_end = find_matching_pair(arr_start, '[', ']');
            if (arr_end) {
                const char *p = arr_start + 1;
                while (p < arr_end && g_drive_count < MAX_DRIVES) {
                    while (p < arr_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
                    if (p >= arr_end || *p == ']') break;
                    if (*p == '{') {
                        const char *obj_end = find_matching_pair(p, '{', '}');
                        if (!obj_end || obj_end > arr_end) break;

                        size_t len = (size_t)(obj_end - p + 1);
                        char item[1024];
                        if (len < sizeof(item)) {
                            memcpy(item, p, len);
                            item[len] = '\0';

                            pkg_drive_t *d = &g_drives[g_drive_count++];
                            memset(d, 0, sizeof(*d));
                            extract_json_field(item, "id", d->id, sizeof(d->id));
                            extract_json_field(item, "label", d->label, sizeof(d->label));
                            extract_json_field(item, "path", d->path, sizeof(d->path));
                            extract_json_field(item, "type", d->type, sizeof(d->type));

                            char val[64];
                            extract_json_field(item, "mounted", val, sizeof(val));
                            d->mounted = atoi(val);
                            extract_json_field(item, "pkg_count", val, sizeof(val));
                            d->pkg_count = (size_t)strtoul(val, NULL, 10);
                            d->clickable = (d->pkg_count > 0);
                        }
                        p = obj_end + 1;
                    } else {
                        p++;
                    }
                }
            }
        }
    }

    /* Parse files */
    g_scanned_file_count = 0;
    const char *files_key = strstr(buf, "\"files\"");
    if (files_key) {
        const char *arr_start = strchr(files_key, '[');
        if (arr_start) {
            const char *arr_end = find_matching_pair(arr_start, '[', ']');
            if (arr_end) {
                const char *p = arr_start + 1;
                while (p < arr_end) {
                    while (p < arr_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
                    if (p >= arr_end || *p == ']') break;
                    if (*p == '{') {
                        const char *obj_end = find_matching_pair(p, '{', '}');
                        if (!obj_end || obj_end > arr_end) break;

                        size_t len = (size_t)(obj_end - p + 1);
                        char item[1024];
                        if (len < sizeof(item)) {
                            memcpy(item, p, len);
                            item[len] = '\0';

                            char fpath[512], val[64];
                            extract_json_field(item, "path", fpath, sizeof(fpath));
                            uint64_t fsz = 0, fmt = 0;
                            extract_json_field(item, "file_size", val, sizeof(val));
                            if (val[0]) fsz = (uint64_t)strtoull(val, NULL, 10);
                            extract_json_field(item, "mtime", val, sizeof(val));
                            if (val[0]) fmt = (uint64_t)strtoull(val, NULL, 10);

                            if (fpath[0] != '\0') {
                                add_scanned_file_locked(fpath, fsz, fmt);
                            }
                        }
                        p = obj_end + 1;
                    } else {
                        p++;
                    }
                }
            }
        }
    }

    /* Parse packages */
    g_package_count = 0;
    const char *pkgs_key = strstr(buf, "\"packages\"");
    if (pkgs_key) {
        const char *arr_start = strchr(pkgs_key, '[');
        if (arr_start) {
            const char *arr_end = find_matching_pair(arr_start, '[', ']');
            if (arr_end) {
                const char *p = arr_start + 1;
                while (p < arr_end && g_package_count < MAX_PACKAGES) {
                    while (p < arr_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
                    if (p >= arr_end || *p == ']') break;
                    if (*p == '{') {
                        const char *obj_end = find_matching_pair(p, '{', '}');
                        if (!obj_end || obj_end > arr_end) break;

                        size_t len = (size_t)(obj_end - p + 1);
                        char *item = (char *)malloc(len + 1);
                        if (item) {
                            memcpy(item, p, len);
                            item[len] = '\0';

                            pkg_detail_t *pkg = &g_packages[g_package_count++];
                            memset(pkg, 0, sizeof(*pkg));
                            extract_json_field(item, "path", pkg->path, sizeof(pkg->path));
                            extract_json_field(item, "filename", pkg->filename, sizeof(pkg->filename));
                            extract_json_field(item, "title_id", pkg->title_id, sizeof(pkg->title_id));
                            extract_json_field(item, "title_name", pkg->title_name, sizeof(pkg->title_name));
                            extract_json_field(item, "localized_titles", pkg->localized_titles, sizeof(pkg->localized_titles));
                            extract_json_field(item, "default_language", pkg->default_language, sizeof(pkg->default_language));
                            extract_json_field(item, "content_id", pkg->content_id, sizeof(pkg->content_id));
                            extract_json_field(item, "app_version", pkg->app_version, sizeof(pkg->app_version));
                            extract_json_field(item, "pkg_type_str", pkg->pkg_type_str, sizeof(pkg->pkg_type_str));
                            extract_json_field(item, "category", pkg->category, sizeof(pkg->category));
                            extract_json_field(item, "blurhash", pkg->blurhash, sizeof(pkg->blurhash));

                            char val[64];
                            extract_json_field(item, "file_size", val, sizeof(val));
                            if (val[0]) pkg->file_size = (uint64_t)strtoull(val, NULL, 10);

                            extract_json_field(item, "total_pkg_size", val, sizeof(val));
                            if (val[0]) pkg->total_pkg_size = (uint64_t)strtoull(val, NULL, 10);
                            else pkg->total_pkg_size = pkg->file_size;

                            extract_json_field(item, "icon_offset", val, sizeof(val));
                            if (val[0]) pkg->icon_offset = (uint64_t)strtoull(val, NULL, 10);

                            extract_json_field(item, "icon_size", val, sizeof(val));
                            if (val[0]) pkg->icon_size = (uint32_t)strtoul(val, NULL, 10);

                            extract_json_field(item, "has_icon", val, sizeof(val));
                            pkg->has_icon = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

                            extract_json_field(item, "is_valid", val, sizeof(val));
                            pkg->is_valid = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

                            extract_json_field(item, "is_multipart", val, sizeof(val));
                            pkg->is_multipart = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);

                            extract_json_field(item, "part_index", val, sizeof(val));
                            if (val[0]) pkg->part_index = (uint32_t)strtoul(val, NULL, 10);

                            extract_json_field(item, "total_parts", val, sizeof(val));
                            if (val[0]) pkg->total_parts = (uint32_t)strtoul(val, NULL, 10);

                            extract_json_field(item, "pkg_type", val, sizeof(val));
                            if (val[0]) pkg->pkg_type = (pkg_type_t)atoi(val);

                            extract_json_field(item, "mtime", val, sizeof(val));
                            if (val[0]) pkg->mtime = (uint64_t)strtoull(val, NULL, 10);

                            free(item);
                        }
                        p = obj_end + 1;
                    } else {
                        p++;
                    }
                }
            }
        }
    }
    free(buf);

    if (g_package_count > 1) {
        qsort(g_packages, g_package_count, sizeof(pkg_detail_t), compare_pkg_by_title_name);
    }
    return 0;
}

int pkg_scanner_has_manifest(void) {
    const char *cache_dir = pkg_cache_get_dir();
    if (!cache_dir || cache_dir[0] == '\0') return 0;
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", cache_dir);
    struct stat st;
    if (stat(manifest_path, &st) == 0 && st.st_size > 10) {
        return 1;
    }
    return 0;
}

void pkg_scanner_get_status(pkg_scan_status_t *out) {
    if (!out) return;
    pthread_mutex_lock(&g_scan_status_mutex);
    memcpy(out, &g_scan_status, sizeof(pkg_scan_status_t));
    pthread_mutex_unlock(&g_scan_status_mutex);
}

char *pkg_scanner_status_to_json(void) {
    pkg_scan_status_t st;
    pkg_scanner_get_status(&st);

    double progress = 0.0;
    if (st.total_files > 0) {
        progress = ((double)st.processed_files / (double)st.total_files) * 100.0;
        if (progress > 100.0) progress = 100.0;
    }

    char buf[1024];
    char esc_file[512] = {0};
    char esc_drive[128] = {0};

    size_t eidx = 0;
    for (size_t i = 0; st.current_file[i] && eidx + 2 < sizeof(esc_file); i++) {
        if (st.current_file[i] == '"' || st.current_file[i] == '\\') {
            esc_file[eidx++] = '\\';
        }
        esc_file[eidx++] = st.current_file[i];
    }
    eidx = 0;
    for (size_t i = 0; st.current_drive[i] && eidx + 2 < sizeof(esc_drive); i++) {
        if (st.current_drive[i] == '"' || st.current_drive[i] == '\\') {
            esc_drive[eidx++] = '\\';
        }
        esc_drive[eidx++] = st.current_drive[i];
    }

    snprintf(buf, sizeof(buf),
             "{\"is_scanning\":%s,\"total_files\":%zu,\"processed_files\":%zu,"
             "\"current_drive\":\"%s\",\"current_file\":\"%s\",\"progress\":%.1f}",
             st.is_scanning ? "true" : "false",
             st.total_files,
             st.processed_files,
             esc_drive,
             esc_file,
             progress);
    return strdup(buf);
}

void pkg_scanner_init(void) {
    pkg_cache_init();

    pthread_mutex_lock(&g_scanner_mutex);
    g_package_count = 0;
    g_drive_count = 0;
    free(g_scanned_files);
    g_scanned_files = NULL;
    g_scanned_file_count = 0;
    g_scanned_file_capacity = 0;
    if (pkg_scanner_has_manifest()) {
        load_manifest_locked();
    }
    pthread_mutex_unlock(&g_scanner_mutex);

    /* Ensure default directory exists */
    struct stat st;
    if (stat(PKG_DEFAULT_DIR, &st) != 0) {
        mkdir(PKG_DEFAULT_DIR, 0777);
    }
}

size_t pkg_scanner_get_count(void) {
    pthread_mutex_lock(&g_scanner_mutex);
    size_t count = g_package_count;
    pthread_mutex_unlock(&g_scanner_mutex);
    return count;
}

int pkg_scanner_get_at(size_t index, pkg_detail_t *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_scanner_mutex);
    if (index >= g_package_count) {
        pthread_mutex_unlock(&g_scanner_mutex);
        return -1;
    }
    memcpy(out, &g_packages[index], sizeof(pkg_detail_t));
    pthread_mutex_unlock(&g_scanner_mutex);
    return 0;
}

size_t pkg_scanner_get_drive_count(void) {
    pthread_mutex_lock(&g_scanner_mutex);
    size_t count = g_drive_count;
    pthread_mutex_unlock(&g_scanner_mutex);
    return count;
}

int pkg_scanner_get_drive_at(size_t index, pkg_drive_t *out) {
    if (!out) return -1;
    pthread_mutex_lock(&g_scanner_mutex);
    if (index >= g_drive_count) {
        pthread_mutex_unlock(&g_scanner_mutex);
        return -1;
    }
    memcpy(out, &g_drives[index], sizeof(pkg_drive_t));
    pthread_mutex_unlock(&g_scanner_mutex);
    return 0;
}

int pkg_scanner_find_by_path(const char *path, pkg_detail_t *out) {
    if (!path || !out) return -1;
    pthread_mutex_lock(&g_scanner_mutex);
    for (size_t i = 0; i < g_package_count; i++) {
        if (strcmp(g_packages[i].path, path) == 0) {
            memcpy(out, &g_packages[i], sizeof(pkg_detail_t));
            pthread_mutex_unlock(&g_scanner_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_scanner_mutex);
    return -1;
}

int pkg_scanner_set_blurhash(const char *path, const char *blurhash) {
    if (!path || !blurhash) return -1;
    pthread_mutex_lock(&g_scanner_mutex);
    for (size_t i = 0; i < g_package_count; i++) {
        if (strcmp(g_packages[i].path, path) == 0) {
            strncpy(g_packages[i].blurhash, blurhash, sizeof(g_packages[i].blurhash) - 1);
            g_packages[i].blurhash[sizeof(g_packages[i].blurhash) - 1] = '\0';
            pthread_mutex_unlock(&g_scanner_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_scanner_mutex);
    return -1;
}

static int is_package_file(const char *name) {
    if (!name || name[0] == '.') return 0;

    uint32_t part_num = 0;
    if (multipart_is_part_filename(name, 0, &part_num)) {
        /* Only Part 1 is listed in UI as a multi-part package card.
         * Subsequent parts (.part2, .pkg.part2, etc.) are hidden from the UI. */
        return (part_num == 1);
    }

    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (strcasecmp(dot, ".pkg") == 0) return 1;
    if (strcasecmp(dot, ".001") == 0) return 1;
    return 0;
}

static int is_drive_mounted(const char *path) {
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return 0;

#if defined(__Prospero__) || defined(PS5_BUILD)
    /* In test/mock environment, directory existence is sufficient */
    if (getenv("PKG_USB_PREFIX") || getenv("PKG_DISC_DIR") || getenv("PKG_SCAN_DIR")) {
        return 1;
    }

    struct statfs sfs;
    if (statfs(path, &sfs) == 0) {
        /* When a drive is actually mounted, sfs.f_mntonname matches path */
        if (strcmp(sfs.f_mntonname, path) == 0) {
            return 1;
        }
    }
    return 0;
#else
    return 1;
#endif
}

static size_t count_pkg_files(const char *dir_path, int recursive, int depth) {
    if (depth > 5) return 0;
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISREG(st.st_mode) && is_package_file(entry->d_name)) {
            count++;
        } else if (S_ISDIR(st.st_mode) && recursive) {
            count += count_pkg_files(full_path, 1, depth + 1);
        }
    }
    closedir(d);
    return count;
}

static int parse_pkg_entry(const char *full_path, const char *filename,
                           uint64_t file_size, uint64_t mtime,
                           pkg_detail_t *out_detail) {
    if (!full_path || !filename || !out_detail) return -1;

    char checksum[64] = {0};
    int parsed = 0;

    if (pkg_cache_calc_checksum(full_path, checksum, sizeof(checksum)) == 0) {
        if (pkg_cache_lookup(checksum, out_detail) == 0) {
            strncpy(out_detail->path, full_path, sizeof(out_detail->path) - 1);
            out_detail->path[sizeof(out_detail->path) - 1] = '\0';
            strncpy(out_detail->filename, filename, sizeof(out_detail->filename) - 1);
            out_detail->filename[sizeof(out_detail->filename) - 1] = '\0';
            out_detail->file_size = file_size;
            if (out_detail->total_pkg_size == 0) {
                out_detail->total_pkg_size = out_detail->file_size;
            }
            out_detail->mtime = mtime;

            if (out_detail->has_icon &&
                (out_detail->blurhash[0] == '\0' ||
                 strlen(out_detail->blurhash) < BLURHASH_UPGRADE_MIN_LEN) &&
                out_detail->icon_offset > 0 && out_detail->icon_size > 0 &&
                out_detail->icon_size < 10 * 1024 * 1024) {
                uint8_t *heal_buf = NULL;
                size_t heal_sz = 0;
                if (pkg_parser_get_icon(out_detail->path, out_detail->icon_offset,
                                        out_detail->icon_size,
                                        &heal_buf, &heal_sz) == 0 &&
                    heal_buf && heal_sz > 0) {
                    char heal_bh[64] = {0};
                    if (icon_compute_blurhash(heal_buf, heal_sz,
                                              heal_bh, sizeof(heal_bh)) == 0) {
                        strncpy(out_detail->blurhash, heal_bh, sizeof(out_detail->blurhash) - 1);
                        pkg_cache_update_meta(checksum, out_detail);
                    }
                    free(heal_buf);
                }
            }
            parsed = 1;
        }
    }

    if (!parsed) {
        if (pkg_parser_parse(full_path, out_detail) == 0) {
            parsed = 1;
            if (checksum[0] != '\0') {
                pkg_cache_save(checksum, out_detail);
            }
        }
    }

    if (!parsed) {
        /* Fallback: include package even if metadata extraction failed */
        memset(out_detail, 0, sizeof(*out_detail));
        strncpy(out_detail->path, full_path, sizeof(out_detail->path) - 1);
        out_detail->path[sizeof(out_detail->path) - 1] = '\0';
        strncpy(out_detail->filename, filename, sizeof(out_detail->filename) - 1);
        out_detail->filename[sizeof(out_detail->filename) - 1] = '\0';
        strncpy(out_detail->title_id, "UNKNOWN", sizeof(out_detail->title_id) - 1);
        strncpy(out_detail->title_name, "Unknown Package", sizeof(out_detail->title_name) - 1);
        out_detail->file_size = file_size;
        out_detail->total_pkg_size = file_size;
        out_detail->mtime = mtime;
        out_detail->is_valid = 1;
    }

    return (!out_detail->is_multipart || out_detail->part_index == 1) ? 0 : 1;
}

static void process_pkg_file(const char *full_path, const char *filename,
                             const struct stat *st, const char *drive_label) {
    add_scanned_file_locked(full_path, (uint64_t)st->st_size, (uint64_t)st->st_mtime);

    pthread_mutex_lock(&g_scan_status_mutex);
    g_scan_status.processed_files++;
    strncpy(g_scan_status.current_file, filename, sizeof(g_scan_status.current_file) - 1);
    if (drive_label && drive_label[0] != '\0') {
        strncpy(g_scan_status.current_drive, drive_label, sizeof(g_scan_status.current_drive) - 1);
    }
    pthread_mutex_unlock(&g_scan_status_mutex);

    if (g_package_count >= MAX_PACKAGES) return;

    pkg_detail_t detail;
    int rc = parse_pkg_entry(full_path, filename, (uint64_t)st->st_size, (uint64_t)st->st_mtime, &detail);
    if (rc == 0) {
        memcpy(&g_packages[g_package_count++], &detail, sizeof(pkg_detail_t));
    }
}

typedef struct {
    const char *drive_label;
} smb_scan_ctx_t;

static void smb_pkg_scan_cb(const char *smb_url, const char *filename,
                            uint64_t file_size, uint32_t mtime,
                            void *user_data) {
    smb_scan_ctx_t *ctx = (smb_scan_ctx_t *)user_data;
    /* total_files is pre-counted upfront (see pkg_scanner_scan). Only grow
       it here if the pre-count was unavailable (0) or if unexpected extra
       files appear beyond the pre-counted total. */
    pthread_mutex_lock(&g_scan_status_mutex);
    if (g_scan_status.total_files == 0) {
        g_scan_status.total_files = 1;
    } else if (g_scan_status.processed_files >= g_scan_status.total_files) {
        g_scan_status.total_files = g_scan_status.processed_files + 1;
    }
    pthread_mutex_unlock(&g_scan_status_mutex);

    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_size = (off_t)file_size;
    st.st_mtime = (time_t)mtime;
    process_pkg_file(smb_url, filename, &st, ctx ? ctx->drive_label : "SMB");
}

/* Scans root directory of a drive (depth 0, non-recursive) */
static size_t scan_dir_root(const char *dir_path, const char *drive_label) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    size_t prev_count = g_package_count;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISREG(st.st_mode) && is_package_file(entry->d_name)) {
            process_pkg_file(full_path, entry->d_name, &st, drive_label);
        }
    }
    closedir(d);
    return g_package_count - prev_count;
}

static void scan_dir_recursive(const char *dir_path, int depth, const char *drive_label) {
    if (depth > 5) return;
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(full_path, depth + 1, drive_label);
        } else if (S_ISREG(st.st_mode) && is_package_file(entry->d_name)) {
            process_pkg_file(full_path, entry->d_name, &st, drive_label);
        }
    }
    closedir(d);
}

static void add_drive_entry(const char *id, const char *label, const char *path,
                            const char *type, int mounted, size_t pkg_count) {
    if (g_drive_count >= MAX_DRIVES) return;
    pkg_drive_t *d = &g_drives[g_drive_count++];
    memset(d, 0, sizeof(*d));
    strncpy(d->id, id, sizeof(d->id) - 1);
    strncpy(d->label, label, sizeof(d->label) - 1);
    strncpy(d->path, path, sizeof(d->path) - 1);
    strncpy(d->type, type, sizeof(d->type) - 1);
    d->mounted = mounted;
    d->pkg_count = pkg_count;
    d->clickable = (pkg_count > 0);
}

int pkg_scanner_scan(void) {
    pthread_mutex_lock(&g_scan_active_mutex);
    pthread_mutex_lock(&g_scanner_mutex);
    g_package_count = 0;
    g_drive_count = 0;
    g_scanned_file_count = 0;

    const char *env_dir = getenv("PKG_SCAN_DIR");
    const char *env_disc = getenv("PKG_DISC_DIR");

    /* Pre-count total package files across all mounted drives for progress reporting */
    size_t total_expected = 0;
    if (env_dir && env_dir[0] != '\0') {
        total_expected += count_pkg_files(env_dir, 1, 0);
        if (env_disc && env_disc[0] != '\0' && is_drive_mounted(env_disc)) {
            total_expected += count_pkg_files(env_disc, 1, 0);
        }
    } else {
        const char *usb_prefix = getenv("PKG_USB_PREFIX");
        if (!usb_prefix || usb_prefix[0] == '\0') usb_prefix = "/mnt/usb";
        for (int i = 0; i < 8; i++) {
            char usb_path[64];
            snprintf(usb_path, sizeof(usb_path), "%s%d", usb_prefix, i);
            if (is_drive_mounted(usb_path)) {
                total_expected += count_pkg_files(usb_path, 0, 0);
                char usb_pkg_dir[128];
                snprintf(usb_pkg_dir, sizeof(usb_pkg_dir), "%s/pkg", usb_path);
                total_expected += count_pkg_files(usb_pkg_dir, 1, 0);
            }
        }
        const char *disc_dir = getenv("PKG_DISC_DIR");
        if (!disc_dir || disc_dir[0] == '\0') disc_dir = "/mnt/disc";
        if (is_drive_mounted(disc_dir)) {
            total_expected += count_pkg_files(disc_dir, 0, 0);
            char disc_pkg_dir[256];
            snprintf(disc_pkg_dir, sizeof(disc_pkg_dir), "%s/pkg", disc_dir);
            total_expected += count_pkg_files(disc_pkg_dir, 1, 0);
        }
        if (is_drive_mounted(PKG_DEFAULT_DIR)) {
            total_expected += count_pkg_files(PKG_DEFAULT_DIR, 1, 0);
        }
    }

    /* Pre-count .pkg files across all enabled SMB shares (readdir only,
       no parsing) so progress reflects real totals (e.g. 1/15, not 7/7). */
    {
        app_settings_t smb_settings;
        pkg_cache_get_settings(&smb_settings);
        for (int i = 0; i < smb_settings.smb_share_count; i++) {
            const smb_share_config_t *scfg = &smb_settings.smb_shares[i];
            if (!scfg->enabled || scfg->server[0] == '\0' || scfg->share[0] == '\0') continue;
            int smb_pkgs = smb_client_count_pkg_files(scfg);
            if (smb_pkgs > 0) {
                total_expected += (size_t)smb_pkgs;
            }
        }
    }

    pthread_mutex_lock(&g_scan_status_mutex);
    memset(&g_scan_status, 0, sizeof(g_scan_status));
    g_scan_status.is_scanning = 1;
    g_scan_status.total_files = total_expected;
    g_scan_status.processed_files = 0;
    pthread_mutex_unlock(&g_scan_status_mutex);

    /* If PKG_SCAN_DIR is set (e.g. during test suites or dev), register it */
    if (env_dir && env_dir[0] != '\0') {
        size_t prev_pkg = g_package_count;
        scan_dir_recursive(env_dir, 0, env_dir);
        size_t count = g_package_count - prev_pkg;
        add_drive_entry("usb0", "USB Drive 0", env_dir, "usb", 1, count);

        if (env_disc && env_disc[0] != '\0' && is_drive_mounted(env_disc)) {
            prev_pkg = g_package_count;
            scan_dir_recursive(env_disc, 0, env_disc);
            count = g_package_count - prev_pkg;
            add_drive_entry("disc", "Blu-ray Disc", env_disc, "disc", 1, count);
        }
    } else {
        const char *usb_prefix = getenv("PKG_USB_PREFIX");
        if (!usb_prefix || usb_prefix[0] == '\0') usb_prefix = "/mnt/usb";

        /* 1. Check mounted USB drives /mnt/usb[0-7] */
        for (int i = 0; i < 8; i++) {
            char usb_path[64];
            snprintf(usb_path, sizeof(usb_path), "%s%d", usb_prefix, i);

            if (is_drive_mounted(usb_path)) {
                size_t prev_pkg = g_package_count;
                char usb_pkg_dir[128];
                snprintf(usb_pkg_dir, sizeof(usb_pkg_dir), "%s/pkg", usb_path);

                char id[32], label[64];
                snprintf(id, sizeof(id), "usb%d", i);
                snprintf(label, sizeof(label), "USB Drive %d", i);

                /* Scan root directory only */
                scan_dir_root(usb_path, label);
                /* Plus recursive scan inside pkg folder, if present */
                scan_dir_recursive(usb_pkg_dir, 0, label);
                size_t count = g_package_count - prev_pkg;

                add_drive_entry(id, label, usb_path, "usb", 1, count);
            }
        }

        /* 2. Check Disc /mnt/disc */
        const char *disc_dir = getenv("PKG_DISC_DIR");
        if (!disc_dir || disc_dir[0] == '\0') disc_dir = "/mnt/disc";

        if (is_drive_mounted(disc_dir)) {
            size_t prev_pkg = g_package_count;
            /* Disc root only (non-recursive) */
            scan_dir_root(disc_dir, "Blu-ray Disc");
            /* Plus recursive scan inside pkg folder, if present */
            char disc_pkg_dir[256];
            snprintf(disc_pkg_dir, sizeof(disc_pkg_dir), "%s/pkg", disc_dir);
            scan_dir_recursive(disc_pkg_dir, 0, "Blu-ray Disc");
            size_t count = g_package_count - prev_pkg;
            add_drive_entry("disc", "Blu-ray Disc", disc_dir, "disc", 1, count);
        }

        /* 3. Check /data/pkg (Internal Storage) if packages are present */
        if (is_drive_mounted(PKG_DEFAULT_DIR)) {
            size_t prev_pkg = g_package_count;
            scan_dir_recursive(PKG_DEFAULT_DIR, 0, "Internal Storage");
            size_t count = g_package_count - prev_pkg;
            if (count > 0) {
                add_drive_entry("internal", "Internal Storage", PKG_DEFAULT_DIR, "internal", 1, count);
            }
        }
    }

    /* 4. Check configured SMB shares */
    app_settings_t smb_settings;
    pkg_cache_get_settings(&smb_settings);
    for (int i = 0; i < smb_settings.smb_share_count; i++) {
        const smb_share_config_t *scfg = &smb_settings.smb_shares[i];
        if (!scfg->enabled || scfg->server[0] == '\0' || scfg->share[0] == '\0') {
            continue;
        }

        char s_id[32], s_label[64], share_root[512];
        if (scfg->id[0] != '\0') {
            strncpy(s_id, scfg->id, sizeof(s_id) - 1);
            s_id[sizeof(s_id) - 1] = '\0';
        } else {
            snprintf(s_id, sizeof(s_id), "smb%d", i);
        }

        if (scfg->label[0] != '\0') {
            strncpy(s_label, scfg->label, sizeof(s_label) - 1);
            s_label[sizeof(s_label) - 1] = '\0';
        } else {
            snprintf(s_label, sizeof(s_label), "SMB: %.58s", scfg->share);
        }

        if (scfg->port && scfg->port != 445) {
            snprintf(share_root, sizeof(share_root), "smb://%s:%d/%s", scfg->server, scfg->port, scfg->share);
        } else {
            snprintf(share_root, sizeof(share_root), "smb://%s/%s", scfg->server, scfg->share);
        }

        smb_scan_ctx_t sctx;
        sctx.drive_label = s_label;

        size_t prev_pkg = g_package_count;
        int sres = smb_client_scan_share(scfg, smb_pkg_scan_cb, &sctx);
        size_t count = g_package_count - prev_pkg;

        if (sres >= 0) {
            add_drive_entry(s_id, s_label, share_root, "smb", 1, count);
        } else {
            add_drive_entry(s_id, s_label, share_root, "smb", 0, 0);
        }
    }

    if (g_package_count > 1) {
        qsort(g_packages, g_package_count, sizeof(pkg_detail_t), compare_pkg_by_title_name);
    }

    save_manifest_locked();

    int total_packages = (int)g_package_count;

    pthread_mutex_lock(&g_scan_status_mutex);
    g_scan_status.is_scanning = 0;
    g_scan_status.processed_files = g_scan_status.total_files;
    pthread_mutex_unlock(&g_scan_status_mutex);

    pthread_mutex_unlock(&g_scanner_mutex);
    pthread_mutex_unlock(&g_scan_active_mutex);
    return total_packages;
}

typedef struct {
    char path[512];
    char filename[256];
    uint64_t file_size;
    uint64_t mtime;
} quick_file_entry_t;

typedef struct {
    quick_file_entry_t *entries;
    size_t count;
    size_t capacity;
} quick_file_list_t;

static void quick_smb_pkg_cb(const char *smb_url, const char *filename,
                             uint64_t file_size, uint32_t mtime,
                             void *user_data) {
    if (!is_package_file(filename)) return;
    quick_file_list_t *list = (quick_file_list_t *)user_data;
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 32 : list->capacity * 2;
        quick_file_entry_t *new_arr = (quick_file_entry_t *)realloc(list->entries, new_cap * sizeof(quick_file_entry_t));
        if (!new_arr) return;
        list->entries = new_arr;
        list->capacity = new_cap;
    }
    quick_file_entry_t *e = &list->entries[list->count++];
    strncpy(e->path, smb_url, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    strncpy(e->filename, filename, sizeof(e->filename) - 1);
    e->filename[sizeof(e->filename) - 1] = '\0';
    e->file_size = file_size;
    e->mtime = (uint64_t)mtime;
}

static void collect_local_files_quick(const char *dir_path, int recursive, int depth, quick_file_list_t *list) {
    if (depth > 5) return;
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode) && recursive) {
            collect_local_files_quick(full_path, 1, depth + 1, list);
        } else if (S_ISREG(st.st_mode) && is_package_file(entry->d_name)) {
            if (list->count >= list->capacity) {
                size_t new_cap = list->capacity == 0 ? 32 : list->capacity * 2;
                quick_file_entry_t *new_arr = (quick_file_entry_t *)realloc(list->entries, new_cap * sizeof(quick_file_entry_t));
                if (!new_arr) {
                    closedir(d);
                    return;
                }
                list->entries = new_arr;
                list->capacity = new_cap;
            }
            quick_file_entry_t *e = &list->entries[list->count++];
            strncpy(e->path, full_path, sizeof(e->path) - 1);
            e->path[sizeof(e->path) - 1] = '\0';
            strncpy(e->filename, entry->d_name, sizeof(e->filename) - 1);
            e->filename[sizeof(e->filename) - 1] = '\0';
            e->file_size = (uint64_t)st.st_size;
            e->mtime = (uint64_t)st.st_mtime;
        }
    }
    closedir(d);
}

static int scan_quick_single_source(const char *drive_id, const char *drive_label,
                                    const char *drive_path, const char *drive_type,
                                    int is_smb, const smb_share_config_t *smb_cfg) {
    quick_file_list_t cur_files;
    memset(&cur_files, 0, sizeof(cur_files));

    if (is_smb) {
        if (!smb_cfg || !smb_cfg->enabled) {
            return 0;
        }
        int res = smb_client_scan_share(smb_cfg, quick_smb_pkg_cb, &cur_files);
        if (res < 0) {
            /* Share offline / network unreachable: retain existing cached entries without purging */
            free(cur_files.entries);
            return 0;
        }
    } else {
        if (!is_drive_mounted(drive_path)) {
            pthread_mutex_lock(&g_scanner_mutex);
            int had_pkgs = 0;
            size_t write_idx = 0;
            for (size_t i = 0; i < g_package_count; i++) {
                if (pkg_matches_drive_path(g_packages[i].path, drive_path)) {
                    had_pkgs = 1;
                } else {
                    if (write_idx != i) {
                        memcpy(&g_packages[write_idx], &g_packages[i], sizeof(pkg_detail_t));
                    }
                    write_idx++;
                }
            }
            g_package_count = write_idx;

            remove_scanned_files_for_drive_locked(drive_path);

            int changed_unmount = had_pkgs;
            for (size_t d = 0; d < g_drive_count; d++) {
                if (strcmp(g_drives[d].path, drive_path) == 0 || strcmp(g_drives[d].id, drive_id) == 0) {
                    if (g_drives[d].mounted) changed_unmount = 1;
                    g_drives[d].mounted = 0;
                    g_drives[d].pkg_count = 0;
                    g_drives[d].clickable = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&g_scanner_mutex);
            return changed_unmount;
        }

        const char *env_dir = getenv("PKG_SCAN_DIR");
        if (env_dir && strcmp(drive_path, env_dir) == 0) {
            collect_local_files_quick(env_dir, 1, 0, &cur_files);
        } else if (strcmp(drive_type, "internal") == 0) {
            collect_local_files_quick(drive_path, 1, 0, &cur_files);
        } else {
            /* Root non-recursive */
            collect_local_files_quick(drive_path, 0, 0, &cur_files);
            /* /pkg subfolder recursive */
            char pkg_subdir[512];
            snprintf(pkg_subdir, sizeof(pkg_subdir), "%s/pkg", drive_path);
            collect_local_files_quick(pkg_subdir, 1, 0, &cur_files);
        }
    }

    /* STEP 1: Check if file list matches recorded scanned files */
    pthread_mutex_lock(&g_scanner_mutex);

    size_t prev_file_count = 0;
    for (size_t i = 0; i < g_scanned_file_count; i++) {
        if (pkg_matches_drive_path(g_scanned_files[i].path, drive_path)) {
            prev_file_count++;
        }
    }

    size_t exist_pkg_count = 0;
    for (size_t i = 0; i < g_package_count; i++) {
        if (pkg_matches_drive_path(g_packages[i].path, drive_path)) {
            exist_pkg_count++;
        }
    }

    int drive_was_mounted = 0;
    for (size_t d = 0; d < g_drive_count; d++) {
        if (strcmp(g_drives[d].path, drive_path) == 0 || strcmp(g_drives[d].id, drive_id) == 0) {
            if (g_drives[d].mounted) drive_was_mounted = 1;
            break;
        }
    }

    int identical = 0;
    if (cur_files.count == prev_file_count) {
        if (cur_files.count == 0) {
            if (drive_was_mounted) {
                identical = 1;
            }
        } else {
            identical = 1;
            for (size_t f = 0; f < cur_files.count; f++) {
                int found = 0;
                for (size_t i = 0; i < g_scanned_file_count; i++) {
                    if (pkg_matches_drive_path(g_scanned_files[i].path, drive_path) &&
                        strcmp(cur_files.entries[f].path, g_scanned_files[i].path) == 0 &&
                        cur_files.entries[f].file_size == g_scanned_files[i].file_size &&
                        cur_files.entries[f].mtime == g_scanned_files[i].mtime) {
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    identical = 0;
                    break;
                }
            }
        }
    }

    if (identical) {
        /* File list is identical: skip re-parsing completely */
        int found_drive = 0;
        for (size_t d = 0; d < g_drive_count; d++) {
            if (strcmp(g_drives[d].path, drive_path) == 0 || strcmp(g_drives[d].id, drive_id) == 0) {
                g_drives[d].mounted = 1;
                g_drives[d].pkg_count = exist_pkg_count;
                g_drives[d].clickable = (exist_pkg_count > 0);
                found_drive = 1;
                break;
            }
        }
        if (!found_drive && g_drive_count < MAX_DRIVES) {
            add_drive_entry(drive_id, drive_label, drive_path, drive_type, 1, exist_pkg_count);
        }
        pthread_mutex_unlock(&g_scanner_mutex);
        free(cur_files.entries);
        return 0;
    }

    /* STEP 2: File list changed.
       Match existing packages from cache, identify new/modified files to parse. */
    pkg_detail_t *updated_pkgs = NULL;
    if (cur_files.count > 0) {
        updated_pkgs = (pkg_detail_t *)malloc(cur_files.count * sizeof(pkg_detail_t));
        if (!updated_pkgs) {
            pthread_mutex_unlock(&g_scanner_mutex);
            free(cur_files.entries);
            return 0; /* Keep catalog intact on allocation failure */
        }
    }
    size_t updated_count = 0;

    int *needs_parsing = NULL;
    if (cur_files.count > 0) {
        needs_parsing = (int *)calloc(cur_files.count, sizeof(int));
    }

    for (size_t f = 0; f < cur_files.count; f++) {
        quick_file_entry_t *file = &cur_files.entries[f];
        int matched = 0;
        for (size_t i = 0; i < g_package_count; i++) {
            if (pkg_matches_drive_path(g_packages[i].path, drive_path) &&
                strcmp(file->path, g_packages[i].path) == 0 &&
                file->file_size == g_packages[i].file_size &&
                file->mtime == g_packages[i].mtime) {
                if (updated_pkgs && updated_count < cur_files.count) {
                    memcpy(&updated_pkgs[updated_count++], &g_packages[i], sizeof(pkg_detail_t));
                }
                matched = 1;
                break;
            }
        }
        if (!matched) {
            int already_scanned_and_skipped = 0;
            for (size_t i = 0; i < g_scanned_file_count; i++) {
                if (pkg_matches_drive_path(g_scanned_files[i].path, drive_path) &&
                    strcmp(file->path, g_scanned_files[i].path) == 0 &&
                    file->file_size == g_scanned_files[i].file_size &&
                    file->mtime == g_scanned_files[i].mtime) {
                    already_scanned_and_skipped = 1;
                    break;
                }
            }
            if (!already_scanned_and_skipped && needs_parsing) {
                needs_parsing[f] = 1;
            }
        }
    }

    /* Release lock while parsing new or modified files */
    pthread_mutex_unlock(&g_scanner_mutex);

    for (size_t f = 0; f < cur_files.count; f++) {
        if (needs_parsing && needs_parsing[f] && updated_pkgs && updated_count < cur_files.count) {
            quick_file_entry_t *file = &cur_files.entries[f];
            pkg_detail_t new_detail;
            int rc = parse_pkg_entry(file->path, file->filename, file->file_size, file->mtime, &new_detail);
            if (rc == 0) {
                memcpy(&updated_pkgs[updated_count++], &new_detail, sizeof(pkg_detail_t));
            }
        }
    }
    free(needs_parsing);

    /* STEP 3: Re-acquire lock to atomically commit updated packages & scanned files */
    pthread_mutex_lock(&g_scanner_mutex);

    size_t write_idx = 0;
    for (size_t i = 0; i < g_package_count; i++) {
        if (!pkg_matches_drive_path(g_packages[i].path, drive_path)) {
            if (write_idx != i) {
                memcpy(&g_packages[write_idx], &g_packages[i], sizeof(pkg_detail_t));
            }
            write_idx++;
        }
    }
    g_package_count = write_idx;

    if (updated_pkgs) {
        for (size_t u = 0; u < updated_count && g_package_count < MAX_PACKAGES; u++) {
            memcpy(&g_packages[g_package_count++], &updated_pkgs[u], sizeof(pkg_detail_t));
        }
        free(updated_pkgs);
    }

    /* Update recorded scanned files for this drive */
    remove_scanned_files_for_drive_locked(drive_path);
    for (size_t f = 0; f < cur_files.count; f++) {
        add_scanned_file_locked(cur_files.entries[f].path,
                                cur_files.entries[f].file_size,
                                cur_files.entries[f].mtime);
    }

    int found_drive = 0;
    for (size_t d = 0; d < g_drive_count; d++) {
        if (strcmp(g_drives[d].path, drive_path) == 0 || strcmp(g_drives[d].id, drive_id) == 0) {
            g_drives[d].mounted = 1;
            g_drives[d].pkg_count = updated_count;
            g_drives[d].clickable = (updated_count > 0);
            found_drive = 1;
            break;
        }
    }
    if (!found_drive && g_drive_count < MAX_DRIVES) {
        if (strcmp(drive_type, "internal") != 0 || updated_count > 0) {
            add_drive_entry(drive_id, drive_label, drive_path, drive_type, 1, updated_count);
        }
    }

    pthread_mutex_unlock(&g_scanner_mutex);
    free(cur_files.entries);
    return 1;
}

int pkg_scanner_scan_quick(const char *drive_id_or_path, int *out_changed) {
    if (out_changed) *out_changed = 0;

    if (pthread_mutex_trylock(&g_scan_active_mutex) != 0) {
        /* Another scan is active; do not collide */
        return (int)pkg_scanner_get_count();
    }

    int total_changed = 0;
    app_settings_t smb_settings;
    pkg_cache_get_settings(&smb_settings);

    const char *env_dir = getenv("PKG_SCAN_DIR");
    const char *env_disc = getenv("PKG_DISC_DIR");

    if (env_dir && env_dir[0] != '\0') {
        if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
            strcmp(drive_id_or_path, "usb0") == 0 || strcmp(drive_id_or_path, env_dir) == 0) {
            total_changed += scan_quick_single_source("usb0", "USB Drive 0", env_dir, "usb", 0, NULL);
        }
        if (env_disc && env_disc[0] != '\0' && is_drive_mounted(env_disc)) {
            if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
                strcmp(drive_id_or_path, "disc") == 0 || strcmp(drive_id_or_path, env_disc) == 0) {
                total_changed += scan_quick_single_source("disc", "Blu-ray Disc", env_disc, "disc", 0, NULL);
            }
        }
    } else {
        const char *usb_prefix = getenv("PKG_USB_PREFIX");
        if (!usb_prefix || usb_prefix[0] == '\0') usb_prefix = "/mnt/usb";

        for (int i = 0; i < 8; i++) {
            char usb_id[32], usb_label[64], usb_path[64];
            snprintf(usb_id, sizeof(usb_id), "usb%d", i);
            snprintf(usb_label, sizeof(usb_label), "USB Drive %d", i);
            snprintf(usb_path, sizeof(usb_path), "%s%d", usb_prefix, i);

            if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
                strcmp(drive_id_or_path, usb_id) == 0 || strcmp(drive_id_or_path, usb_path) == 0) {
                total_changed += scan_quick_single_source(usb_id, usb_label, usb_path, "usb", 0, NULL);
            }
        }

        const char *disc_dir = getenv("PKG_DISC_DIR");
        if (!disc_dir || disc_dir[0] == '\0') disc_dir = "/mnt/disc";
        if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
            strcmp(drive_id_or_path, "disc") == 0 || strcmp(drive_id_or_path, disc_dir) == 0) {
            total_changed += scan_quick_single_source("disc", "Blu-ray Disc", disc_dir, "disc", 0, NULL);
        }

        if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
            strcmp(drive_id_or_path, "internal") == 0 || strcmp(drive_id_or_path, PKG_DEFAULT_DIR) == 0) {
            total_changed += scan_quick_single_source("internal", "Internal Storage", PKG_DEFAULT_DIR, "internal", 0, NULL);
        }
    }

    /* Check SMB shares */
    for (int i = 0; i < smb_settings.smb_share_count; i++) {
        const smb_share_config_t *scfg = &smb_settings.smb_shares[i];
        if (!scfg->enabled || scfg->server[0] == '\0' || scfg->share[0] == '\0') {
            continue;
        }

        char s_id[32], s_label[64], share_root[512];
        if (scfg->id[0] != '\0') {
            strncpy(s_id, scfg->id, sizeof(s_id) - 1);
            s_id[sizeof(s_id) - 1] = '\0';
        } else {
            snprintf(s_id, sizeof(s_id), "smb%d", i);
        }

        if (scfg->label[0] != '\0') {
            strncpy(s_label, scfg->label, sizeof(s_label) - 1);
            s_label[sizeof(s_label) - 1] = '\0';
        } else {
            snprintf(s_label, sizeof(s_label), "SMB: %.58s", scfg->share);
        }

        if (scfg->port && scfg->port != 445) {
            snprintf(share_root, sizeof(share_root), "smb://%s:%d/%s", scfg->server, scfg->port, scfg->share);
        } else {
            snprintf(share_root, sizeof(share_root), "smb://%s/%s", scfg->server, scfg->share);
        }

        if (!drive_id_or_path || drive_id_or_path[0] == '\0' || strcmp(drive_id_or_path, "__all__") == 0 ||
            strcmp(drive_id_or_path, s_id) == 0 || strcmp(drive_id_or_path, share_root) == 0 ||
            pkg_matches_drive_path(drive_id_or_path, share_root)) {
            total_changed += scan_quick_single_source(s_id, s_label, share_root, "smb", 1, scfg);
        }
    }

    if (total_changed > 0) {
        pthread_mutex_lock(&g_scanner_mutex);
        if (g_package_count > 1) {
            qsort(g_packages, g_package_count, sizeof(pkg_detail_t), compare_pkg_by_title_name);
        }
        save_manifest_locked();
        pthread_mutex_unlock(&g_scanner_mutex);
        if (out_changed) *out_changed = 1;
    }

    pthread_mutex_unlock(&g_scan_active_mutex);
    return (int)pkg_scanner_get_count();
}

static int check_part_file(const char *full_path,
                           const uint8_t *package_uuid, const char *pkg_filename,
                           uint32_t part_index, char *out_path, size_t out_max,
                           uint32_t *out_detected_part) {
    uint32_t fn_part = 0;
    /* Only inspect files matching a multi-part file extension case-insensitively */
    if (!multipart_is_part_filename(full_path, 0, &fn_part)) {
        return 0;
    }

    multipart_header_t hdr;
    if (multipart_read_header(full_path, &hdr) != 0) {
        return 0;
    }

    int match = 0;
    if (package_uuid && memcmp(hdr.package_uuid, package_uuid, 16) == 0) {
        match = 1;
    } else if (pkg_filename && pkg_filename[0] != '\0' &&
               strcasecmp(hdr.pkg_filename, pkg_filename) == 0) {
        match = 1;
    } else if (!package_uuid && (!pkg_filename || pkg_filename[0] == '\0')) {
        match = 1;
    }
    if (!match) {
        return 0;
    }

    if (out_detected_part && *out_detected_part == 0 && hdr.part_index != part_index) {
        *out_detected_part = hdr.part_index;
    }
    if (hdr.part_index == part_index) {
        strncpy(out_path, full_path, out_max - 1);
        out_path[out_max - 1] = '\0';
        return 1;
    }
    return 0;
}

static int scan_find_part_recursive(const char *dir_path, int depth,
                                    const uint8_t *package_uuid, const char *pkg_filename,
                                    uint32_t part_index, char *out_path, size_t out_max,
                                    uint32_t *out_detected_part) {
    if (depth > 5) return -1;
    DIR *d = opendir(dir_path);
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (scan_find_part_recursive(full_path, depth + 1, package_uuid, pkg_filename,
                                        part_index, out_path, out_max, out_detected_part) == 0) {
                closedir(d);
                return 0;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (check_part_file(full_path, package_uuid, pkg_filename, part_index,
                                out_path, out_max, out_detected_part)) {
                closedir(d);
                return 0;
            }
        }
    }
    closedir(d);
    return -1;
}

/* Search one drive location with catalog rules: regular files directly in
   base_dir (non-recursive), plus a recursive descent into a pkg/ subfolder
   only. Anything elsewhere on the drive is out of scope. */
static int scan_find_part_in_location(const char *base_dir,
                                      const uint8_t *package_uuid, const char *pkg_filename,
                                      uint32_t part_index, char *out_path, size_t out_max,
                                      uint32_t *out_detected_part) {
    DIR *d = opendir(base_dir);
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISREG(st.st_mode)) {
            if (check_part_file(full_path, package_uuid, pkg_filename, part_index,
                                out_path, out_max, out_detected_part)) {
                closedir(d);
                return 0;
            }
        } else if (S_ISDIR(st.st_mode) && strcmp(entry->d_name, "pkg") == 0) {
            if (scan_find_part_recursive(full_path, 0, package_uuid, pkg_filename,
                                        part_index, out_path, out_max, out_detected_part) == 0) {
                closedir(d);
                return 0;
            }
        }
    }
    closedir(d);
    return -1;
}

int pkg_scanner_find_part_ex(const uint8_t *package_uuid, const char *pkg_filename,
                            uint32_t part_index, char *out_path, size_t out_max,
                            uint32_t *out_detected_part) {
    if (!out_path || out_max == 0) return -1;
    if (out_detected_part) *out_detected_part = 0;

    const char *env_dir = getenv("PKG_SCAN_DIR");
    const char *env_disc = getenv("PKG_DISC_DIR");

    if (env_dir && env_dir[0] != '\0') {
        if (scan_find_part_in_location(env_dir, package_uuid, pkg_filename, part_index, out_path, out_max, out_detected_part) == 0) {
            return 0;
        }
        if (env_disc && env_disc[0] != '\0') {
            if (scan_find_part_in_location(env_disc, package_uuid, pkg_filename, part_index, out_path, out_max, out_detected_part) == 0) {
                return 0;
            }
        }
    }

    /* Disc location first (most common for disc swap multi-part): root files,
       then recursive inside its pkg/ subfolder. Then USBs, then internal. */
    if (scan_find_part_in_location("/mnt/disc", package_uuid, pkg_filename, part_index, out_path, out_max, out_detected_part) == 0) {
        return 0;
    }

    for (int i = 0; i < 8; i++) {
        char usb_path[64];
        snprintf(usb_path, sizeof(usb_path), "/mnt/usb%d", i);
        if (is_drive_mounted(usb_path)) {
            if (scan_find_part_in_location(usb_path, package_uuid, pkg_filename, part_index, out_path, out_max, out_detected_part) == 0) {
                return 0;
            }
        }
    }

    /* Internal storage lives inside the pkg folder already: same rules. */
    if (scan_find_part_in_location(PKG_DEFAULT_DIR, package_uuid, pkg_filename, part_index, out_path, out_max, out_detected_part) == 0) {
        return 0;
    }

    return -1;
}

int pkg_scanner_find_part(const uint8_t *package_uuid, const char *pkg_filename,
                         uint32_t part_index, char *out_path, size_t out_max) {
    return pkg_scanner_find_part_ex(package_uuid, pkg_filename, part_index, out_path, out_max, NULL);
}

static void escape_json_string(const char *src, char *dst, size_t dst_max) {
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
            /* Skip non-printable control chars */
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

char *pkg_scanner_drives_to_json(void) {
    pthread_mutex_lock(&g_scanner_mutex);

    size_t buf_size = 64 + g_drive_count * 512;
    char *json = (char *)malloc(buf_size);
    if (!json) {
        pthread_mutex_unlock(&g_scanner_mutex);
        return NULL;
    }

    size_t pos = 0;
    int w = snprintf(json + pos, buf_size - pos, "[");
    if (w > 0 && (size_t)w < buf_size - pos) pos += (size_t)w;

    int first = 1;
    for (size_t i = 0; i < g_drive_count; i++) {
        pkg_drive_t *d = &g_drives[i];
        if (!d->mounted) continue;

        char esc_id[64], esc_label[128], esc_path[512], esc_type[32];
        escape_json_string(d->id, esc_id, sizeof(esc_id));
        escape_json_string(d->label, esc_label, sizeof(esc_label));
        escape_json_string(d->path, esc_path, sizeof(esc_path));
        escape_json_string(d->type, esc_type, sizeof(esc_type));

        w = snprintf(json + pos, buf_size - pos,
            "%s{"
            "\"id\":\"%s\","
            "\"label\":\"%s\","
            "\"path\":\"%s\","
            "\"type\":\"%s\","
            "\"mounted\":true,"
            "\"pkg_count\":%zu,"
            "\"clickable\":%s"
            "}",
            (first ? "" : ","),
            esc_id,
            esc_label,
            esc_path,
            esc_type,
            d->pkg_count,
            d->clickable ? "true" : "false");

        first = 0;
        if (w < 0 || (size_t)w >= buf_size - pos) break;
        pos += (size_t)w;
    }

    if (pos < buf_size) {
        snprintf(json + pos, buf_size - pos, "]");
    }
    pthread_mutex_unlock(&g_scanner_mutex);
    return json;
}

char *pkg_scanner_packages_for_drive_to_json_ex(const char *drive_id_or_path, const char *accept_language) {
    pthread_mutex_lock(&g_scanner_mutex);

    /* Find matching drive path if drive_id_or_path was provided */
    char target_path[256] = {0};
    if (drive_id_or_path && drive_id_or_path[0] != '\0' && strcmp(drive_id_or_path, "__all__") != 0) {
        for (size_t d = 0; d < g_drive_count; d++) {
            if (strcmp(g_drives[d].id, drive_id_or_path) == 0 ||
                strcmp(g_drives[d].path, drive_id_or_path) == 0) {
                strncpy(target_path, g_drives[d].path, sizeof(target_path) - 1);
                break;
            }
        }
        /* If not matched in g_drives, treat drive_id_or_path as literal directory prefix */
        if (target_path[0] == '\0') {
            strncpy(target_path, drive_id_or_path, sizeof(target_path) - 1);
        }
    }

    size_t buf_size = 64 + g_package_count * 4096;
    char *json = (char *)malloc(buf_size);
    if (!json) {
        pthread_mutex_unlock(&g_scanner_mutex);
        return NULL;
    }

    size_t pos = 0;
    int w = snprintf(json + pos, buf_size - pos, "[");
    if (w > 0 && (size_t)w < buf_size - pos) {
        pos += (size_t)w;
    }

    int emitted = 0;
    for (size_t i = 0; i < g_package_count; i++) {
        pkg_detail_t *pkg = &g_packages[i];
        if (pkg->filename[0] == '.') {
            continue;
        }

        if (target_path[0] != '\0') {
            /* Package must be located within target drive directory */
            size_t tlen = strlen(target_path);
            if (strncmp(pkg->path, target_path, tlen) != 0 ||
                (target_path[tlen - 1] != '/' && pkg->path[tlen] != '/' && pkg->path[tlen] != '\0')) {
                continue;
            }
        }

        char installed_version[32] = {0};
        int is_installed = app_info_check_installed(pkg->title_id, installed_version, sizeof(installed_version));
        int is_dlc_installed = 0;
        if (pkg->pkg_type == PKG_TYPE_DLC && pkg->content_id[0] != '\0') {
            is_dlc_installed = app_info_check_dlc_installed(pkg->title_id, pkg->content_id);
        }

        char leftover_desc[128] = {0};
        int has_leftover = 0;
        if (!is_installed) {
            has_leftover = app_info_check_has_leftover(pkg->title_id, leftover_desc, sizeof(leftover_desc));
        }

        char partial_desc[128] = {0};
        int is_partially_installed = 0;
        if (!is_installed) {
            is_partially_installed = app_info_check_partially_installed(pkg->title_id, partial_desc, sizeof(partial_desc));
        }

        pkg_install_eligibility_t eligibility;
        evaluate_install_eligibility(pkg, is_installed, installed_version, is_dlc_installed,
                                     has_leftover, is_partially_installed, &eligibility);
        int can_install = eligibility.can_install;
        const char *disabled_reason = eligibility.disabled_reason;

        char esc_path[1024];
        char esc_filename[512];
        char esc_title_id[64];
        char esc_title_name[512];
        char esc_def_lang[64];
        char esc_content_id[128];
        char esc_app_version[64];
        char esc_pkg_type[32];
        char esc_category[32];
        char esc_installed_ver[64];
        char esc_disabled_reason[256];
        char esc_leftover_desc[256];
        char esc_partial_desc[256];
        char esc_blurhash[128];

        char display_title[PKG_TITLE_NAME_LEN];
        strncpy(display_title, pkg->title_name, sizeof(display_title) - 1);
        display_title[sizeof(display_title) - 1] = '\0';
        if (accept_language && accept_language[0] && pkg->localized_titles[0] == '{') {
            pkg_parser_resolve_localized_title(pkg->localized_titles, pkg->default_language,
                                               accept_language, display_title, sizeof(display_title));
        }

        const char *loc_json_raw = (pkg->localized_titles[0] == '{') ? pkg->localized_titles : "{}";

        escape_json_string(pkg->path, esc_path, sizeof(esc_path));
        escape_json_string(pkg->filename, esc_filename, sizeof(esc_filename));
        escape_json_string(pkg->title_id, esc_title_id, sizeof(esc_title_id));
        escape_json_string(display_title, esc_title_name, sizeof(esc_title_name));
        escape_json_string(pkg->default_language, esc_def_lang, sizeof(esc_def_lang));
        escape_json_string(pkg->content_id, esc_content_id, sizeof(esc_content_id));
        escape_json_string(pkg->app_version, esc_app_version, sizeof(esc_app_version));
        escape_json_string(pkg->pkg_type_str[0] ? pkg->pkg_type_str : "unknown", esc_pkg_type, sizeof(esc_pkg_type));
        escape_json_string(pkg->category, esc_category, sizeof(esc_category));
        escape_json_string(installed_version, esc_installed_ver, sizeof(esc_installed_ver));
        escape_json_string(disabled_reason, esc_disabled_reason, sizeof(esc_disabled_reason));
        escape_json_string(leftover_desc, esc_leftover_desc, sizeof(esc_leftover_desc));
        escape_json_string(partial_desc, esc_partial_desc, sizeof(esc_partial_desc));
        escape_json_string(pkg->blurhash, esc_blurhash, sizeof(esc_blurhash));

        if (pos + 1024 >= buf_size) {
            break;
        }

        w = snprintf(json + pos, buf_size - pos,
            "%s{"
            "\"path\":\"%s\","
            "\"filename\":\"%s\","
            "\"title_id\":\"%s\","
            "\"title_name\":\"%s\","
            "\"localized_titles\":%s,"
            "\"default_language\":\"%s\","
            "\"content_id\":\"%s\","
            "\"app_version\":\"%s\","
            "\"file_size\":%llu,"
            "\"total_pkg_size\":%llu,"
            "\"has_icon\":%s,"
            "\"is_multipart\":%s,"
            "\"part_index\":%u,"
            "\"total_parts\":%u,"
            "\"pkg_type\":\"%s\","
            "\"category\":\"%s\","
            "\"mtime\":%llu,"
            "\"is_installed\":%s,"
            "\"installed_version\":\"%s\","
            "\"is_dlc_installed\":%s,"
            "\"has_leftover\":%s,"
            "\"leftover_desc\":\"%s\","
            "\"is_partially_installed\":%s,"
            "\"partial_desc\":\"%s\","
            "\"can_install\":%s,"
            "\"install_disabled_reason\":\"%s\","
            "\"blurhash\":\"%s\""
            "}",
            (emitted > 0 ? "," : ""),
            esc_path,
            esc_filename,
            esc_title_id,
            esc_title_name,
            loc_json_raw,
            esc_def_lang,
            esc_content_id,
            esc_app_version,
            (unsigned long long)pkg->file_size,
            (unsigned long long)(pkg->total_pkg_size > 0 ? pkg->total_pkg_size : pkg->file_size),
            pkg->has_icon ? "true" : "false",
            pkg->is_multipart ? "true" : "false",
            pkg->part_index,
            pkg->total_parts,
            esc_pkg_type,
            esc_category,
            (unsigned long long)pkg->mtime,
            is_installed ? "true" : "false",
            esc_installed_ver,
            is_dlc_installed ? "true" : "false",
            has_leftover ? "true" : "false",
            esc_leftover_desc,
            is_partially_installed ? "true" : "false",
            esc_partial_desc,
            can_install ? "true" : "false",
            esc_disabled_reason,
            esc_blurhash);

        if (w < 0 || (size_t)w >= buf_size - pos) {
            break;
        }
        pos += (size_t)w;
        emitted++;
    }

    if (pos < buf_size) {
        snprintf(json + pos, buf_size - pos, "]");
    }
    pthread_mutex_unlock(&g_scanner_mutex);
    return json;
}

char *pkg_scanner_packages_for_drive_to_json(const char *drive_id_or_path) {
    return pkg_scanner_packages_for_drive_to_json_ex(drive_id_or_path, NULL);
}

char *pkg_scanner_to_json(void) {
    return pkg_scanner_packages_for_drive_to_json(NULL);
}
