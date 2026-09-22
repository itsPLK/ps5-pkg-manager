/*
 * PKG Manager - HTTP Server & REST API Dispatcher
 *
 * Implements the libmicrohttpd request handler, REST API routes,
 * and embedded asset delivery.
 */

#include "http_server.h"
#include "pkg_scanner.h"
#include "pkg_cache.h"
#include "icon_blurhash.h"
#include "installer.h"
#include "assets_index_html.h"
#include "assets_cache_appcache.h"
#include "assets_favicon_svg.h"
#include "assets_icon_png.h"
#include "version.h"
#include "smb_client.h"
#include "leftovers.h"
#include "app_diag.h"
#include "app_installer.h"
#include "ws_upload.h"
#include "ws_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <microhttpd.h>

#define RESPONSE_BUFFER_SIZE 65536
#define MAX_POST_BODY_SIZE (64 * 1024)

typedef struct {
    char *data;
    size_t size;
    int oversize;
} post_state_t;

static struct MHD_Daemon *g_daemon = NULL;
static volatile int g_server_running = 0;

static void add_cors_headers(struct MHD_Response *resp) {
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers", "Content-Type, Range");
    MHD_add_response_header(resp, "Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges");
}

static int extract_json_string_value(const char *json, const char *key, char *out, size_t out_max) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return -1;

    const char *colon = strchr(p + strlen(pattern), ':');
    if (!colon) return -1;

    const char *q = strchr(colon, '"');
    if (!q) return -1;
    q++;

    size_t i = 0;
    while (*q && *q != '"' && i + 1 < out_max) {
        if (*q == '\\' && *(q + 1)) {
            q++;
        }
        out[i++] = *q++;
    }
    out[i] = '\0';
    return 0;
}

static void http_request_completed(void *cls, struct MHD_Connection *conn,
                                   void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)conn; (void)toe;
    if (*con_cls != NULL) {
        post_state_t *ps = (post_state_t *)*con_cls;
        if (ps->data) {
            free(ps->data);
        }
        free(ps);
        *con_cls = NULL;
    }
}

static enum MHD_Result handle_options(struct MHD_Connection *conn) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
    add_cors_headers(resp);
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* Validate PNG magic so we never serve stale/garbage bytes as image/png
   (e.g. scanner offsets that went stale after the PKG changed on disk). */
static int icon_has_png_magic(const uint8_t *data, size_t size) {
    static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || size < 8) return 0;
    return memcmp(data, png_magic, sizeof(png_magic)) == 0;
}

static void json_str_esc(const char *src, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t d = 0;
    for (size_t s = 0; src[s] && d + 6 < dst_sz; s++) {
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
        } else if (c < 32) {
            d += snprintf(dst + d, dst_sz - d, "\\u%04x", c);
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

static enum MHD_Result http_on_request(void *cls, struct MHD_Connection *conn,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls) {
    (void)cls; (void)version;

    /* Handle CORS Preflight (OPTIONS) */
    if (strcmp(method, "OPTIONS") == 0) {
        return handle_options(conn);
    }

    /* Initial call for POST request: allocate post_state_t */
    if (strcmp(method, "POST") == 0 && *con_cls == NULL) {
        post_state_t *ps = (post_state_t *)calloc(1, sizeof(post_state_t));
        if (!ps) return MHD_NO;
        *con_cls = ps;
        return MHD_YES;
    }

    /* Process upload data for POST request (capped to prevent OOM DoS) */
    if (strcmp(method, "POST") == 0 && *upload_data_size != 0) {
        post_state_t *ps = (post_state_t *)*con_cls;
        if (!ps) return MHD_NO;
        size_t body_limit = strcmp(url, "/api/upload/icon") == 0
            ? WS_DIRECT_ICON_MAX : MAX_POST_BODY_SIZE - 1;
        if (ps->oversize || *upload_data_size > body_limit - ps->size) {
            ps->oversize = 1;
            *upload_data_size = 0;
            return MHD_YES;
        }
        char *new_data = (char *)realloc(ps->data, ps->size + *upload_data_size + 1);
        if (!new_data) return MHD_NO;
        memcpy(new_data + ps->size, upload_data, *upload_data_size);
        ps->size += *upload_data_size;
        new_data[ps->size] = '\0';
        ps->data = new_data;

        *upload_data_size = 0;
        return MHD_YES;
    }

    /* Reject oversized POST bodies with 413 */
    if (strcmp(method, "POST") == 0 && *con_cls != NULL) {
        post_state_t *ps = (post_state_t *)*con_cls;
        if (ps && ps->oversize) {
            static const char err_big[] = "{\"error\":\"Payload Too Large\"}";
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                sizeof(err_big) - 1, (void *)err_big, MHD_RESPMEM_PERSISTENT);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_CONTENT_TOO_LARGE, resp);
            MHD_destroy_response(resp);
            return ret;
        }
    }

    /* ── Direct-install upload sessions ──────────────────────────────
     * Narrow guarded branch: only URLs under /api/upload/ are handled here.
     * With no browser calling these routes, control falls through to the
     * untouched logic below. Chunk bytes travel over the WS listener
     * (:18842, ws_upload.c), never through MHD/Post bodies. */
    if (strncmp(url, "/api/upload/", 12) == 0) {
        if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload/icon") == 0) {
            post_state_t *ps = (post_state_t *)*con_cls;
            const char *owner = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "X-Direct-Owner");
            const char *sid = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "X-Direct-Session");
            int ok = ps && ps->data && owner && sid &&
                ws_direct_set_icon(owner, sid, (const uint8_t *)ps->data, ps->size) == 0;
            const char *body = ok ? "{\"success\":true}" : "{\"success\":false}";
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn,
                ok ? MHD_HTTP_OK : MHD_HTTP_BAD_REQUEST, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* Check local package metadata before offering a direct installation. */
        if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload/check") == 0) {
            post_state_t *ps = (post_state_t *)*con_cls;
            pkg_detail_t pkg = {0};
            char response[512];
            unsigned int code = MHD_HTTP_OK;
            if (!ps || !ps->data ||
                extract_json_string_value(ps->data, "title_id", pkg.title_id, sizeof(pkg.title_id)) != 0 ||
                extract_json_string_value(ps->data, "pkg_type", pkg.pkg_type_str, sizeof(pkg.pkg_type_str)) != 0 ||
                extract_json_string_value(ps->data, "content_id", pkg.content_id, sizeof(pkg.content_id)) != 0 ||
                extract_json_string_value(ps->data, "app_version", pkg.app_version, sizeof(pkg.app_version)) != 0 ||
                !pkg.title_id[0] ||
                (strcmp(pkg.pkg_type_str, "base") != 0 &&
                 strcmp(pkg.pkg_type_str, "update") != 0 &&
                 strcmp(pkg.pkg_type_str, "dlc") != 0) ||
                (strcmp(pkg.pkg_type_str, "dlc") == 0 && !pkg.content_id[0])) {
                code = MHD_HTTP_BAD_REQUEST;
                snprintf(response, sizeof(response), "{\"error\":\"Invalid package metadata\"}");
            } else {
                pkg.pkg_type = strcmp(pkg.pkg_type_str, "update") == 0 ? PKG_TYPE_UPDATE :
                               strcmp(pkg.pkg_type_str, "dlc") == 0 ? PKG_TYPE_DLC : PKG_TYPE_BASE;
                pkg_install_eligibility_t eligibility;
                pkg_scanner_check_install_eligibility(&pkg, &eligibility);
                char reason[256], installed_version[96];
                json_str_esc(eligibility.disabled_reason, reason, sizeof(reason));
                json_str_esc(eligibility.installed_version, installed_version, sizeof(installed_version));
                snprintf(response, sizeof(response),
                         "{\"can_install\":%s,\"install_disabled_reason\":\"%s\","
                         "\"is_installed\":%s,\"installed_version\":\"%s\"}",
                         eligibility.can_install ? "true" : "false", reason,
                         eligibility.is_installed ? "true" : "false", installed_version);
            }
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(response), response, MHD_RESPMEM_MUST_COPY);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, code, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* POST /api/upload/init {"filename":"game.pkg","total":123} */
        if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload/init") == 0) {
            post_state_t *ps = (post_state_t *)*con_cls;
            char fn[256] = {0};
            char owner[65] = {0}, resume_sid[64] = {0};
            char title[256] = {0}, title_id[64] = {0}, version[32] = {0}, kind[16] = {0};
            uint64_t total = 0;
            if (ps && ps->data) {
                extract_json_string_value(ps->data, "filename", fn, sizeof(fn));
                extract_json_string_value(ps->data, "owner", owner, sizeof(owner));
                extract_json_string_value(ps->data, "session_id", resume_sid, sizeof(resume_sid));
                extract_json_string_value(ps->data, "title_name", title, sizeof(title));
                extract_json_string_value(ps->data, "title_id", title_id, sizeof(title_id));
                extract_json_string_value(ps->data, "app_version", version, sizeof(version));
                extract_json_string_value(ps->data, "pkg_type", kind, sizeof(kind));
                const char *tp = strstr(ps->data, "\"total\"");
                if (tp) {
                    tp = strchr(tp + 7, ':');
                    if (tp) total = strtoull(tp + 1, NULL, 10);
                }
            }
            char resp_json[512];
            unsigned int code = MHD_HTTP_OK;
            if (fn[0] == '\0' || total == 0) {
                snprintf(resp_json, sizeof(resp_json),
                         "{\"success\":false,\"error\":\"filename and total required\"}");
                code = MHD_HTTP_BAD_REQUEST;
            } else {
                char sid[64] = {0};
                int rc = ws_direct_init_owned(fn, total, owner, resume_sid,
                                              sid, sizeof(sid));
                if (rc == -2) {
                    snprintf(resp_json, sizeof(resp_json),
                             "{\"success\":false,\"error\":\"Another upload is active\"}");
                    code = MHD_HTTP_CONFLICT;
                } else if (rc != 0) {
                    snprintf(resp_json, sizeof(resp_json),
                             "{\"success\":false,\"error\":\"Cannot start upload session\"}");
                    code = MHD_HTTP_BAD_REQUEST;
                } else {
                    ws_direct_set_metadata(owner, sid, title, title_id, version, kind);
                    /* The spool session exists but chunk bytes travel over
                     * the :18842 listener: refuse loudly if it is down instead
                     * of letting the browser time out against a dead port. */
                    if (!ws_direct_listener_running()) {
                        ws_direct_cancel_session();
                        snprintf(resp_json, sizeof(resp_json),
                                 "{\"success\":false,\"error\":\"Upload socket unavailable\"}");
                        code = MHD_HTTP_INTERNAL_SERVER_ERROR;
                    } else {
                        uint64_t off = ws_live_get_resume_offset();
                        snprintf(resp_json, sizeof(resp_json),
                                 "{\"success\":true,\"session_id\":\"%s\",\"offset\":%llu,\"ws_port\":%d}",
                                 sid, (unsigned long long)off,
                                 ws_direct_listener_port() > 0 ?
                                     ws_direct_listener_port() : WS_DIRECT_DEFAULT_PORT);
                    }
                }
            }
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(resp_json), (void *)resp_json, MHD_RESPMEM_MUST_COPY);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, code, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* GET /api/upload/status */
        if (strcmp(method, "GET") == 0 && strcmp(url, "/api/upload/status") == 0) {
            char st[768] = {0};
            ws_direct_get_status(st, sizeof(st));
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(st), (void *)st, MHD_RESPMEM_MUST_COPY);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* POST /api/upload/finish */
        if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload/finish") == 0) {
            char path[640] = {0};
            char resp_json[1600];
            unsigned int code = MHD_HTTP_OK;
            if (ws_direct_finish_session(path, sizeof(path)) != 0) {
                char st[768] = {0};
                ws_direct_get_status(st, sizeof(st));
                snprintf(resp_json, sizeof(resp_json),
                         "{\"success\":false,\"error\":\"Upload incomplete\",\"status\":%s}", st);
                code = MHD_HTTP_BAD_REQUEST;
            } else {
                char esc[700] = {0};
                json_str_esc(path, esc, sizeof(esc));
                snprintf(resp_json, sizeof(resp_json),
                         "{\"success\":true,\"path\":\"%s\"}", esc);
            }
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(resp_json), (void *)resp_json, MHD_RESPMEM_MUST_COPY);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, code, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* POST /api/upload/cancel */
        if (strcmp(method, "POST") == 0 && strcmp(url, "/api/upload/cancel") == 0) {
            post_state_t *ps = (post_state_t *)*con_cls;
            char owner[65] = {0}, sid[64] = {0};
            if (ps && ps->data) {
                extract_json_string_value(ps->data, "owner", owner, sizeof(owner));
                extract_json_string_value(ps->data, "session_id", sid, sizeof(sid));
            }
            int ok = ws_direct_cancel_owned(owner, sid) == 0;
            const char *ok_resp = ok ? "{\"success\":true}" :
                "{\"success\":false,\"error\":\"Session belongs to another window\"}";
            struct MHD_Response *resp = MHD_create_response_from_buffer(
                strlen(ok_resp), (void *)ok_resp, MHD_RESPMEM_PERSISTENT);
            add_cors_headers(resp);
            MHD_add_response_header(resp, "Content-Type", "application/json");
            enum MHD_Result ret = MHD_queue_response(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        /* Unknown upload sub-path: fall through to 404 below. */
    }

    /* ── GET / or /index.html ──────────────────────────────────── */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0 || strcmp(url, "/index.htm") == 0)) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            assets_index_html_len, (void *)assets_index_html, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
        MHD_add_response_header(resp, "Cache-Control", "no-cache, must-revalidate");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET or HEAD /favicon.svg or /favicon.ico ──────────────── */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
        (strcmp(url, "/favicon.svg") == 0 || strcmp(url, "/favicon.ico") == 0)) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            assets_favicon_svg_len, (void *)assets_favicon_svg, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "image/svg+xml");
        MHD_add_response_header(resp, "Cache-Control", "max-age=604800");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET or HEAD /icon.png or /apple-touch-icon.png ────────── */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
        (strcmp(url, "/icon.png") == 0 || strcmp(url, "/apple-touch-icon.png") == 0 || strcmp(url, "/icon-192.png") == 0)) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            assets_icon_png_len, (void *)assets_icon_png, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "image/png");
        MHD_add_response_header(resp, "Cache-Control", "max-age=604800");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET or HEAD /cache.appcache ───────────────────────────── */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && strcmp(url, "/cache.appcache") == 0) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            assets_cache_appcache_len, (void *)assets_cache_appcache, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "text/cache-manifest");
        MHD_add_response_header(resp, "Cache-Control", "no-cache, must-revalidate");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET or HEAD /api/version (and /version) ────────────────── */
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
        (strcmp(url, "/api/version") == 0 || strcmp(url, "/version") == 0)) {
        const char *ver = PKGMGR_VERSION;
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(ver), (void *)ver, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "text/plain; charset=utf-8");
        MHD_add_response_header(resp, "Cache-Control", "no-cache, must-revalidate");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/drives ───────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/drives") == 0) {
        char *json = pkg_scanner_drives_to_json();
        if (!json) {
            json = strdup("[]");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/packages ─────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/packages") == 0) {
        const char *drive_param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "drive");
        const char *accept_lang = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Accept-Language");
        char *json = pkg_scanner_packages_for_drive_to_json_ex(drive_param, accept_lang);
        if (!json) {
            json = strdup("[]");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/storage ──────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/storage") == 0) {
        uint64_t free_b = 0, total_b = 0, used_b = 0;
        system_get_storage_info(&free_b, &total_b, &used_b);

        uint64_t nvme_free = 0, nvme_total = 0, nvme_used = 0;
        int nvme_avail = (system_get_nvme_storage_info(&nvme_free, &nvme_total, &nvme_used) == 0);

        uint64_t usb_free = 0, usb_total = 0, usb_used = 0;
        int usb_avail = (system_get_usb_storage_info(&usb_free, &usb_total, &usb_used) == 0);

        char buf[768];
        snprintf(buf, sizeof(buf),
                 "{\"free\":%llu,\"total\":%llu,\"used\":%llu,\"path\":\"/data\",\"label\":\"Internal\","
                 "\"internal\":{\"free\":%llu,\"total\":%llu,\"used\":%llu,\"path\":\"/data\",\"label\":\"Internal\"},"
                 "\"nvme\":{\"available\":%s,\"free\":%llu,\"total\":%llu,\"used\":%llu,\"path\":\"/mnt/ext1\",\"label\":\"M.2 NVMe\"},"
                 "\"usb\":{\"available\":%s,\"free\":%llu,\"total\":%llu,\"used\":%llu,\"path\":\"/mnt/ext0\",\"label\":\"USB\"}}",
                 (unsigned long long)free_b, (unsigned long long)total_b, (unsigned long long)used_b,
                 (unsigned long long)free_b, (unsigned long long)total_b, (unsigned long long)used_b,
                 nvme_avail ? "true" : "false",
                 (unsigned long long)nvme_free, (unsigned long long)nvme_total, (unsigned long long)nvme_used,
                 usb_avail ? "true" : "false",
                 (unsigned long long)usb_free, (unsigned long long)usb_total, (unsigned long long)usb_used);
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(buf), (void *)buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/packages/quick-scan ─────────────────────────── */
    if (strcmp(method, "POST") == 0 &&
        (strcmp(url, "/api/packages/quick-scan") == 0 ||
         (strcmp(url, "/api/packages/refresh") == 0 && MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "quick") != NULL))) {
        const char *drive_param = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "drive");
        int changed = 0;
        int count = pkg_scanner_scan_quick(drive_param, &changed);
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"count\":%d,\"changed\":%s}",
                 count, changed ? "true" : "false");
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(buf), (void *)buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/packages/refresh ────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/packages/refresh") == 0) {
        int count = pkg_scanner_scan();
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"count\":%d}", count);
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(buf), (void *)buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/scan/status ──────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/scan/status") == 0) {
        char *json = pkg_scanner_status_to_json();
        if (!json) {
            json = strdup("{\"is_scanning\":false,\"total_files\":0,\"processed_files\":0,\"current_drive\":\"\",\"current_file\":\"\",\"progress\":0.0}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/icon ─────────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/icon") == 0) {
        const char *pkg_path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
        if (pkg_path && strncmp(pkg_path, "live:", 5) == 0) {
            uint8_t *icon_data = NULL;
            size_t icon_size = 0;
            if (ws_direct_get_icon(pkg_path + 5, &icon_data, &icon_size) == 0) {
                struct MHD_Response *resp = MHD_create_response_from_buffer(
                    icon_size, icon_data, MHD_RESPMEM_MUST_FREE);
                add_cors_headers(resp);
                MHD_add_response_header(resp, "Content-Type", "image/png");
                MHD_add_response_header(resp, "Cache-Control", "no-store");
                enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
                MHD_destroy_response(resp);
                return ret;
            }
        }
        if (pkg_path && pkg_path[0] != '\0' && strncmp(pkg_path, "live:", 5) != 0) {
            int is_smb = (strncmp(pkg_path, "smb://", 6) == 0);
            uint8_t *icon_data = NULL;
            size_t icon_size = 0;

            /* 1. SMB fast path: the scanner already knows icon_offset/size, so a
               single direct pread (one SMB connection) suffices. Doing the
               checksum+cache dance first costs 3 connections per icon, which
               melts the server when a Details page fires 10-20 icon requests
               concurrently (timeouts, resets, broken images). */
            if (is_smb) {
                pkg_detail_t detail;
                if (pkg_scanner_find_by_path(pkg_path, &detail) == 0 &&
                    detail.has_icon && detail.icon_offset > 0 && detail.icon_size > 0) {
                    uint8_t *buf = NULL;
                    size_t sz = 0;
                    if (pkg_parser_get_icon(detail.path, detail.icon_offset,
                                            detail.icon_size, &buf, &sz) == 0 &&
                        buf && sz > 0 && icon_has_png_magic(buf, sz)) {
                        icon_data = buf;
                        icon_size = sz;
                    } else {
                        free(buf);
                    }
                }
            }

            /* 2. Disk/SMB metadata cache (cached icon.png). */
            if (!icon_data) {
                uint8_t *cached_icon = NULL;
                size_t cached_sz = 0;
                if (pkg_cache_get_icon(pkg_path, &cached_icon, &cached_sz) == 0 &&
                    cached_icon && cached_sz > 0 && icon_has_png_magic(cached_icon, cached_sz)) {
                    icon_data = cached_icon;
                    icon_size = cached_sz;
                } else {
                    free(cached_icon);
                }
            }

            /* 3. Fresh parse (cheap locally; header re-parse over SMB) + direct read.
               For SMB the scanner entry was already tried in step 1, so go straight
               to re-parsing in case the scanned offsets went stale. */
            if (!icon_data) {
                pkg_detail_t detail;
                int found = 0;
                if (!is_smb && pkg_scanner_find_by_path(pkg_path, &detail) == 0 && detail.has_icon) {
                    found = 1;
                } else if (pkg_parser_parse(pkg_path, &detail) == 0 && detail.has_icon) {
                    found = 1;
                }
                if (found) {
                    uint8_t *buf = NULL;
                    size_t sz = 0;
                    if (pkg_parser_get_icon(detail.path, detail.icon_offset,
                                            detail.icon_size, &buf, &sz) == 0 &&
                        buf && sz > 0 && icon_has_png_magic(buf, sz)) {
                        icon_data = buf;
                        icon_size = sz;
                    } else {
                        free(buf);
                    }
                }
            }

            if (icon_data) {
                /* Lazy BlurHash backfill for old caches predating the hash field.
                   Runs before handing buffer ownership to MHD so icon_data is guaranteed
                   valid. Compute takes ~0.3ms. Updates in-memory entry and persists
                   meta.json only (never re-reads PKG or re-extracts icon.png). */
                {
                    pkg_detail_t detail;
                    if (pkg_scanner_find_by_path(pkg_path, &detail) == 0 &&
                        detail.has_icon &&
                        (detail.blurhash[0] == '\0' ||
                         strlen(detail.blurhash) < BLURHASH_UPGRADE_MIN_LEN)) {
                        char bh[64] = {0};
                        if (icon_compute_blurhash(icon_data, icon_size, bh, sizeof(bh)) == 0) {
                            char checksum[64] = {0};
                            pkg_scanner_set_blurhash(pkg_path, bh);
                            if (pkg_cache_calc_checksum(pkg_path, checksum, sizeof(checksum)) == 0) {
                                pkg_detail_t upd;
                                memcpy(&upd, &detail, sizeof(upd));
                                strncpy(upd.blurhash, bh, sizeof(upd.blurhash) - 1);
                                pkg_cache_update_meta(checksum, &upd);
                            }
                        }
                    }
                }

                /* Versioned URLs (?v=mtime_size) are content-addressed: safe to
                   cache immutably. Unversioned URLs keep the old 1-day TTL. */
                const char *ver = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "v");
                struct MHD_Response *resp = MHD_create_response_from_buffer(
                    icon_size, (void *)icon_data, MHD_RESPMEM_MUST_FREE);
                add_cors_headers(resp);
                MHD_add_response_header(resp, "Content-Type", "image/png");
                if (ver && ver[0] != '\0') {
                    MHD_add_response_header(resp, "Cache-Control", "public, max-age=31536000, immutable");
                } else {
                    MHD_add_response_header(resp, "Cache-Control", "public, max-age=86400");
                }
                enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
                MHD_destroy_response(resp);
                return ret;
            }
        }

        /* 404 Not Found */
        static const char not_found[] = "{\"error\":\"Icon not found\"}";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            sizeof(not_found) - 1, (void *)not_found, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/icon-error ───────────────────────────────────── */
    /* Diagnostic beacon: the frontend reports icons that failed to load even
       after its retry, so browser-side failures show up in the server log. */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/icon-error") == 0) {
        const char *err_path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
        const char *err_info = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "info");
        install_log("[HTTP] WARNING: icon client-failure info='%s' path='%.160s'",
                    err_info ? err_info : "",
                    err_path ? err_path : "");
        static const char ok_resp[] = "{\"ok\":true}";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            sizeof(ok_resp) - 1, (void *)ok_resp, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/settings ─────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/settings") == 0) {
        app_settings_t s;
        pkg_cache_get_settings(&s);

        char shares_json[8192];
        size_t spos = 0;
        shares_json[0] = '\0';
        for (int i = 0; i < s.smb_share_count; i++) {
            const smb_share_config_t *sh = &s.smb_shares[i];
            char e_id[64], e_lbl[128], e_srv[256], e_shr[256], e_pth[512], e_usr[128], e_grp[128];
            json_str_esc(sh->id, e_id, sizeof(e_id));
            json_str_esc(sh->label, e_lbl, sizeof(e_lbl));
            json_str_esc(sh->server, e_srv, sizeof(e_srv));
            json_str_esc(sh->share, e_shr, sizeof(e_shr));
            json_str_esc(sh->path, e_pth, sizeof(e_pth));
            json_str_esc(sh->username, e_usr, sizeof(e_usr));
            json_str_esc(sh->workgroup, e_grp, sizeof(e_grp));

            /* Never emit cleartext passwords: frontend shows blank + has_password.
             * Password changes are accepted via POST; blank POST preserves stored. */
            int w = snprintf(shares_json + spos, sizeof(shares_json) - spos,
                             "%s{"
                             "\"id\":\"%s\","
                             "\"label\":\"%s\","
                             "\"server\":\"%s\","
                             "\"port\":%d,"
                             "\"share\":\"%s\","
                             "\"path\":\"%s\","
                             "\"username\":\"%s\","
                             "\"password\":\"\","
                             "\"has_password\":%s,"
                             "\"workgroup\":\"%s\","
                             "\"is_read_only\":%s,"
                             "\"enabled\":%s"
                             "}",
                             (i > 0 ? "," : ""),
                             e_id, e_lbl, e_srv, sh->port,
                             e_shr, e_pth, e_usr,
                             sh->password[0] != '\0' ? "true" : "false",
                             e_grp,
                             sh->is_read_only ? "true" : "false",
                             sh->enabled ? "true" : "false");
            if (w > 0 && spos + (size_t)w < sizeof(shares_json)) spos += (size_t)w;
        }

        char *buf = (char *)malloc(spos + 1024);
        if (!buf) return MHD_NO;
        snprintf(buf, spos + 1024,
                 "{\"move_installed_to_end\":%s,\"fade_installed_packages\":%s,\"all_sources_mode\":%s,\"pkg_install_debug\":%s,\"smb_shares\":[%s]}",
                 s.move_installed_to_end ? "true" : "false",
                 s.fade_installed_packages ? "true" : "false",
                 s.all_sources_mode ? "true" : "false",
                 s.pkg_install_debug ? "true" : "false", shares_json);
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(buf), (void *)buf, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/settings ────────────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/settings") == 0) {
        post_state_t *ps = (post_state_t *)*con_cls;
        app_settings_t s;
        pkg_cache_get_settings(&s);

        if (ps && ps->data) {
            char *mptr = strstr(ps->data, "\"move_installed_to_end\":");
            if (mptr) {
                if (strncmp(mptr + 24, "false", 5) == 0 || strncmp(mptr + 25, "false", 5) == 0) {
                    s.move_installed_to_end = 0;
                } else if (strncmp(mptr + 24, "true", 4) == 0 || strncmp(mptr + 25, "true", 4) == 0) {
                    s.move_installed_to_end = 1;
                }
            }

            char *fptr = strstr(ps->data, "\"fade_installed_packages\":");
            if (fptr) {
                if (strncmp(fptr + 26, "false", 5) == 0 || strncmp(fptr + 27, "false", 5) == 0) {
                    s.fade_installed_packages = 0;
                } else if (strncmp(fptr + 26, "true", 4) == 0 || strncmp(fptr + 27, "true", 4) == 0) {
                    s.fade_installed_packages = 1;
                }
            }

            char *aptr = strstr(ps->data, "\"all_sources_mode\":");
            if (aptr) {
                if (strncmp(aptr + 19, "true", 4) == 0 || strncmp(aptr + 20, "true", 4) == 0) {
                    s.all_sources_mode = 1;
                } else if (strncmp(aptr + 19, "false", 5) == 0 || strncmp(aptr + 20, "false", 5) == 0) {
                    s.all_sources_mode = 0;
                }
            }

            char *dptr = strstr(ps->data, "\"pkg_install_debug\":");
            if (dptr) {
                if (strncmp(dptr + 20, "true", 4) == 0 || strncmp(dptr + 21, "true", 4) == 0) {
                    s.pkg_install_debug = 1;
                } else if (strncmp(dptr + 20, "false", 5) == 0 || strncmp(dptr + 21, "false", 5) == 0) {
                    s.pkg_install_debug = 0;
                }
            }

            if (strstr(ps->data, "\"smb_shares\"")) {
                app_settings_t old;
                pkg_cache_get_settings(&old);
                pkg_cache_parse_smb_shares(ps->data, s.smb_shares, &s.smb_share_count);
                /* Blank password in POST means "keep stored password" (GET no
                 * longer echoes it). Match by id, else server+share (+ username). */
                for (int i = 0; i < s.smb_share_count; i++) {
                    smb_client_sanitize_config(&s.smb_shares[i]);
                    if (s.smb_shares[i].password[0] != '\0') continue;
                    for (int j = 0; j < old.smb_share_count; j++) {
                        smb_share_config_t old_cfg = old.smb_shares[j];
                        smb_client_sanitize_config(&old_cfg);
                        int same = 0;
                        if (s.smb_shares[i].id[0] != '\0' && old_cfg.id[0] != '\0' &&
                            strcmp(s.smb_shares[i].id, old_cfg.id) == 0) {
                            same = 1;
                        } else if (old_cfg.server[0] != '\0' &&
                                   strcasecmp(s.smb_shares[i].server, old_cfg.server) == 0 &&
                                   strcasecmp(s.smb_shares[i].share, old_cfg.share) == 0 &&
                                   strcasecmp(s.smb_shares[i].username, old_cfg.username) == 0) {
                            same = 1;
                        }
                        if (same && old.smb_shares[j].password[0] != '\0') {
                            strncpy(s.smb_shares[i].password, old.smb_shares[j].password,
                                    sizeof(s.smb_shares[i].password) - 1);
                            s.smb_shares[i].password[sizeof(s.smb_shares[i].password) - 1] = '\0';
                            break;
                        }
                    }
                }
            }
        }

        pkg_cache_set_settings(&s);

        const char ok_resp[] = "{\"success\":true}";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            sizeof(ok_resp) - 1, (void *)ok_resp, MHD_RESPMEM_PERSISTENT);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/smb/test ─────────────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/smb/test") == 0) {
        post_state_t *ps = (post_state_t *)*con_cls;
        smb_share_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.port = SMB_DEFAULT_PORT;
        cfg.enabled = 1;

        if (ps && ps->data) {
            smb_share_config_t parsed_shares[1];
            int count = 0;
            if (strstr(ps->data, "\"smb_shares\"")) {
                pkg_cache_parse_smb_shares(ps->data, parsed_shares, &count);
            } else {
                char *wrap_buf = (char *)malloc(strlen(ps->data) + 64);
                if (wrap_buf) {
                    sprintf(wrap_buf, "{\"smb_shares\":[%s]}", ps->data);
                    pkg_cache_parse_smb_shares(wrap_buf, parsed_shares, &count);
                    free(wrap_buf);
                }
            }
            if (count > 0) {
                memcpy(&cfg, &parsed_shares[0], sizeof(cfg));
            }
        }

        smb_client_sanitize_config(&cfg);

        /* If password or workgroup is blank, try to inherit saved settings */
        if (cfg.password[0] == '\0' || cfg.workgroup[0] == '\0') {
            app_settings_t saved;
            pkg_cache_get_settings(&saved);
            for (int j = 0; j < saved.smb_share_count; j++) {
                smb_share_config_t saved_cfg = saved.smb_shares[j];
                smb_client_sanitize_config(&saved_cfg);
                int same = 0;
                if (cfg.id[0] != '\0' && saved_cfg.id[0] != '\0' &&
                    strcmp(cfg.id, saved_cfg.id) == 0) {
                    same = 1;
                } else if (saved_cfg.server[0] != '\0' &&
                           strcasecmp(cfg.server, saved_cfg.server) == 0 &&
                           strcasecmp(cfg.share, saved_cfg.share) == 0 &&
                           strcasecmp(cfg.username, saved_cfg.username) == 0) {
                    same = 1;
                }
                if (same) {
                    if (cfg.password[0] == '\0' && saved.smb_shares[j].password[0] != '\0') {
                        strncpy(cfg.password, saved.smb_shares[j].password, sizeof(cfg.password) - 1);
                        cfg.password[sizeof(cfg.password) - 1] = '\0';
                    }
                    if (cfg.workgroup[0] == '\0' && saved.smb_shares[j].workgroup[0] != '\0') {
                        strncpy(cfg.workgroup, saved.smb_shares[j].workgroup, sizeof(cfg.workgroup) - 1);
                        cfg.workgroup[sizeof(cfg.workgroup) - 1] = '\0';
                    }
                    break;
                }
            }
        }

        install_log("[HTTP] POST /api/smb/test: starting connection test for host='%s', share='%s', user='%s'",
                    cfg.server, cfg.share, cfg.username[0] ? cfg.username : "(guest)");

        char err_buf[512] = {0};
        int res = smb_client_test_connection(&cfg, err_buf, sizeof(err_buf));
        int success = (res == 0 || res == 1);
        int is_ro = (res == 1) || cfg.is_read_only;

        install_log("[HTTP] POST /api/smb/test: finished with res=%d (success=%s, is_ro=%s, message='%s')",
                    res, success ? "true" : "false", is_ro ? "true" : "false", err_buf);

        char esc_msg[1024] = {0};
        json_str_esc(err_buf, esc_msg, sizeof(esc_msg));

        char resp_json[2048];
        snprintf(resp_json, sizeof(resp_json),
                 "{\"success\":%s,\"message\":\"%s\",\"is_read_only\":%s}",
                 success ? "true" : "false",
                 esc_msg,
                 is_ro ? "true" : "false");

        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(resp_json), (void *)resp_json, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/cache/stats ──────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/cache/stats") == 0) {
        char *json = pkg_cache_get_stats_json();
        if (!json) {
            json = strdup("{\"total_bytes\":0,\"total_count\":0,\"drives\":[]}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/cache/clear ─────────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/cache/clear") == 0) {
        int64_t freed = pkg_cache_clear();

        char buf[128];
        snprintf(buf, sizeof(buf), "{\"success\":true,\"freed_bytes\":%lld}", (long long)freed);
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(buf), (void *)buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/shortcut/install ────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/shortcut/install") == 0) {
        int res = app_installer_force_install();
        char resp_json[128];
        snprintf(resp_json, sizeof(resp_json),
                 "{\"success\":%s,\"title_id\":\"%s\"}",
                 res == 0 ? "true" : "false",
                 PKGMGR_TITLE_ID);
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(resp_json), (void *)resp_json, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/log (and /log) ───────────────────────────────── */
    if (strcmp(method, "GET") == 0 && (strcmp(url, "/api/log") == 0 || strcmp(url, "/log") == 0)) {
        size_t log_sz = 0;
        char *log_data = install_log_get_text(&log_sz);
        if (!log_data) {
            log_data = strdup("No logs recorded yet.\n");
            log_sz = strlen(log_data);
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            log_sz, (void *)log_data, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "text/plain; charset=utf-8");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/install ─────────────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/install") == 0) {
        post_state_t *ps = (post_state_t *)*con_cls;
        char target_path[512] = {0};

        if (ps && ps->data) {
            extract_json_string_value(ps->data, "path", target_path, sizeof(target_path));
        }

        char response_buf[512];
        unsigned int status_code = MHD_HTTP_OK;

        if (target_path[0] == '\0') {
            snprintf(response_buf, sizeof(response_buf),
                     "{\"success\":false,\"error\":\"Missing package path\"}");
            status_code = MHD_HTTP_BAD_REQUEST;
        } else if (strncmp(target_path, "live:", 5) == 0) {
            /* NEW: live RAM session (Direct Install, no disk file). */
            int res = installer_start_live(target_path);
            if (res == 0) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":true,\"message\":\"Live installation started successfully\"}");
            } else if (res == -2) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Another package is currently installing\"}");
                status_code = MHD_HTTP_CONFLICT;
            } else if (res == -10) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Insufficient storage space to install package\"}");
                status_code = MHD_HTTP_BAD_REQUEST;
            } else if (res == -13) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Live install supports single packages only\"}");
                status_code = MHD_HTTP_BAD_REQUEST;
            } else if (res == -14) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Live header timed out, re-upload the package\"}");
                status_code = MHD_HTTP_BAD_REQUEST;
            } else {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Failed to start live install (code %d)\"}", res);
                status_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
            }
        } else {
            int res = installer_start(target_path);
            if (res == 0) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":true,\"message\":\"Installation started successfully\"}");
            } else if (res == -2) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Another package is currently installing\"}");
                status_code = MHD_HTTP_CONFLICT;
            } else if (res == -10) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Insufficient storage space to install package\"}");
                status_code = MHD_HTTP_BAD_REQUEST;
            } else if (res == -11) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Failed to create temporary directory /data/pkgmgr/tmp\"}");
                status_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
            } else if (res == -12) {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Failed to launch installation worker thread\"}");
                status_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
            } else {
                snprintf(response_buf, sizeof(response_buf),
                         "{\"success\":false,\"error\":\"Failed to install package (code %d)\"}", res);
                status_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(response_buf), (void *)response_buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, status_code, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/cancel ──────────────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/cancel") == 0) {
        int res = installer_cancel();
        char response_buf[256];
        if (res == 0) {
            snprintf(response_buf, sizeof(response_buf),
                     "{\"success\":true,\"message\":\"Installation canceled\"}");
        } else {
            snprintf(response_buf, sizeof(response_buf),
                     "{\"success\":false,\"error\":\"No active installation to cancel\"}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(response_buf), (void *)response_buf, MHD_RESPMEM_MUST_COPY);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/poll or /api/status ──────────────────────────── */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(url, "/api/poll") == 0 || strcmp(url, "/api/status") == 0)) {
        char *json = installer_status_to_json();
        if (!json) {
            json = strdup("{\"error\":\"Out of memory\"}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/debug ────────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/debug") == 0) {
        char *report = app_diag_generate_report();
        if (!report) {
            report = strdup("Failed to generate diagnostic report\n");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(report), (void *)report, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "text/plain; charset=utf-8");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── GET /api/leftovers ─────────────────────────────────────── */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/leftovers") == 0) {
        char *json = leftovers_scan_json();
        if (!json) {
            json = strdup("{\"count\":0,\"leftovers\":[]}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(json), (void *)json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* ── POST /api/leftovers/delete ─────────────────────────────── */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/leftovers/delete") == 0) {
        post_state_t *ps = (post_state_t *)*con_cls;
        char title_id[64] = {0};
        if (ps && ps->data) {
            extract_json_string_value(ps->data, "title_id", title_id, sizeof(title_id));
        }

        char *res_json = leftovers_delete_json(title_id);
        if (!res_json) {
            res_json = strdup("{\"success\":false,\"error\":\"Internal error deleting leftovers\"}");
        }
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(res_json), (void *)res_json, MHD_RESPMEM_MUST_FREE);
        add_cors_headers(resp);
        MHD_add_response_header(resp, "Content-Type", "application/json");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }


    /* ── Fallback: 404 Not Found ───────────────────────────────── */
    static const char not_found[] = "{\"error\":\"Not found\"}";
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        sizeof(not_found) - 1, (void *)not_found, MHD_RESPMEM_PERSISTENT);
    add_cors_headers(resp);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, resp);
    MHD_destroy_response(resp);
    return ret;
}

int http_server_start(int port) {
    if (g_daemon) {
        return 0; /* Already running */
    }

    if (port <= 0) {
        port = DEFAULT_HTTP_PORT;
    }

    g_daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_DEBUG,
        port,
        NULL,
        NULL,
        &http_on_request,
        NULL,
        MHD_OPTION_THREAD_STACK_SIZE, (size_t)(1024 * 1024),
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)600,
        MHD_OPTION_NOTIFY_COMPLETED,
        &http_request_completed,
        NULL,
        MHD_OPTION_END);

    if (!g_daemon) {
        return -1;
    }

    g_server_running = 1;
    return 0;
}

void http_server_stop(void) {
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = NULL;
    }
    g_server_running = 0;
}

int http_server_is_running(void) {
    return g_server_running;
}

int http_server_restart_with_delay(int port, unsigned int delay_us) {
    http_server_stop();
    if (delay_us > 0) {
        usleep(delay_us);
    }
    return http_server_start(port);
}

int http_server_restart(int port) {
    return http_server_restart_with_delay(port, 500000);
}
