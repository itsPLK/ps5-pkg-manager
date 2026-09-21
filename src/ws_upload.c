/*
 * PKG Manager - Direct-install WebSocket upload (NEW, ISOLATED module).
 *
 * See include/ws_upload.h. Live RAM streaming: browser pushes .pkg bytes,
 * the PS5 installer pulls them concurrently; nothing touches the disk.
 * Transport + protocol live here; byte storage lives in ws_stream.c.
 * No MHD dependency.
 */

#include "ws_upload.h"
#include "installer.h" /* install_log only; no MHD dependency added */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_TEXT_MAX (16 * 1024)
#define WS_BIN_MSG_MAX (8 * 1024 * 1024)
#define WS_IO_TIMEOUT_SEC 30
#define WS_MAX_TRACKED 16

/* ---------------- live session (RAM only, no disk spool) ----------------
 *
 * Transport + protocol live here; all byte storage lives in ws_stream.c.
 * Session state below is only the listener bookkeeping (live fds).
 * Upload API (init/write/finish/cancel/status) delegates to ws_live_*.
 */

#include "ws_stream.h"

static pthread_mutex_t g_ws_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_live[WS_MAX_TRACKED];

static int g_listen_fd = -1;
static volatile int g_listen_running = 0;
static pthread_t g_listen_thread;
static int g_listen_created = 0;
static int g_listen_port = 0;

/* Uplink: the browser socket that owns the upload. Seek requests from
 * blocked readers are emitted here; all control sends on upload sockets
 * are serialized through g_uplink_mu so interleaved writers can't
 * corrupt the frame stream. Only the authenticated socket is installed
 * here; a second connection cannot take over seeks. */
static int g_uplink_fd = -1;
static pthread_mutex_t g_uplink_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_owner_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_owner[65];
static char g_owner_sid[64];
static char g_owner_filename[256];
static uint64_t g_owner_total;
static char g_meta_title[256], g_meta_id[64], g_meta_version[32], g_meta_kind[16];
static pthread_mutex_t g_icon_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_icon_data;
static size_t g_icon_size;
static char g_icon_sid[64];

static void clear_icon(void) {
    pthread_mutex_lock(&g_icon_mu);
    free(g_icon_data);
    g_icon_data = NULL;
    g_icon_size = 0;
    g_icon_sid[0] = '\0';
    pthread_mutex_unlock(&g_icon_mu);
}

static void live_init(void) {
    static int once = 0;
    if (!once) {
        once = 1;
        for (int i = 0; i < WS_MAX_TRACKED; i++) g_live[i] = -1;
    }
}

static void live_add(int fd) {
    live_init();
    pthread_mutex_lock(&g_ws_mu);
    for (int i = 0; i < WS_MAX_TRACKED; i++) {
        if (g_live[i] < 0) { g_live[i] = fd; break; }
    }
    pthread_mutex_unlock(&g_ws_mu);
}

static void live_remove(int fd) {
    pthread_mutex_lock(&g_ws_mu);
    for (int i = 0; i < WS_MAX_TRACKED; i++) {
        if (g_live[i] == fd) g_live[i] = -1;
    }
    pthread_mutex_unlock(&g_ws_mu);
}

/* ---------------- public upload API (live RAM backend) ---------------- */

int ws_direct_session_active(void) {
    return ws_live_session_active();
}

int ws_direct_init_session(const char *filename, uint64_t total_size,
                           char *out_session_id, size_t sid_max) {
    if (!filename || !filename[0] || total_size == 0 || total_size > WS_DIRECT_MAX_TOTAL)
        return -1;
    int rc = ws_live_create(filename, total_size, 0, out_session_id, sid_max);
    if (rc != 0) return rc;
    ws_direct_ensure_listener();
    return 0;
}

int ws_direct_owner_matches(const char *owner, const char *sid) {
    if (!owner || !sid || !owner[0] || !sid[0]) return 0;
    pthread_mutex_lock(&g_owner_mu);
    int ok = g_owner[0] && strcmp(owner, g_owner) == 0 &&
             strcmp(sid, g_owner_sid) == 0 && ws_live_check_id(sid);
    pthread_mutex_unlock(&g_owner_mu);
    return ok;
}

int ws_direct_init_owned(const char *filename, uint64_t total_size,
                         const char *owner, const char *resume_sid,
                         char *out_session_id, size_t sid_max) {
    if (!owner || strlen(owner) < 32 || strlen(owner) > 64 ||
        !filename || !filename[0] || !total_size || total_size > WS_DIRECT_MAX_TOTAL ||
        !out_session_id || sid_max < WS_DIRECT_SESSION_ID_MAX)
        return -1;
    pthread_mutex_lock(&g_owner_mu);
    if (ws_live_session_active()) {
        int same = g_owner[0] && resume_sid && resume_sid[0] &&
                   strcmp(owner, g_owner) == 0 &&
                   strcmp(resume_sid, g_owner_sid) == 0 &&
                   strcmp(filename, g_owner_filename) == 0 &&
                   total_size == g_owner_total && ws_live_check_id(resume_sid);
        if (same && out_session_id && sid_max)
            snprintf(out_session_id, sid_max, "%s", g_owner_sid);
        pthread_mutex_unlock(&g_owner_mu);
        return same ? 0 : -2;
    }
    int rc = ws_direct_init_session(filename, total_size, out_session_id, sid_max);
    if (rc == 0) {
        clear_icon();
        snprintf(g_owner, sizeof(g_owner), "%s", owner);
        snprintf(g_owner_sid, sizeof(g_owner_sid), "%s", out_session_id);
        snprintf(g_owner_filename, sizeof(g_owner_filename), "%s", filename);
        g_owner_total = total_size;
        g_meta_title[0] = g_meta_id[0] = g_meta_version[0] = g_meta_kind[0] = '\0';
    }
    pthread_mutex_unlock(&g_owner_mu);
    return rc;
}

void ws_direct_set_metadata(const char *owner, const char *sid,
                            const char *title, const char *title_id,
                            const char *version, const char *kind) {
    if (!owner || !sid) return;
    pthread_mutex_lock(&g_owner_mu);
    if (g_owner[0] && strcmp(owner, g_owner) == 0 &&
        strcmp(sid, g_owner_sid) == 0 && ws_live_check_id(sid)) {
        snprintf(g_meta_title, sizeof(g_meta_title), "%s", title ? title : "");
        snprintf(g_meta_id, sizeof(g_meta_id), "%s", title_id ? title_id : "");
        snprintf(g_meta_version, sizeof(g_meta_version), "%s", version ? version : "");
        snprintf(g_meta_kind, sizeof(g_meta_kind), "%s", kind ? kind : "");
    }
    pthread_mutex_unlock(&g_owner_mu);
}

int ws_direct_get_metadata(const char *sid, char *title, size_t title_max,
                           char *title_id, size_t id_max, char *version,
                           size_t version_max, char *kind, size_t kind_max) {
    if (!sid) return -1;
    pthread_mutex_lock(&g_owner_mu);
    int ok = g_owner[0] && strcmp(sid, g_owner_sid) == 0 && ws_live_check_id(sid);
    if (ok) {
        if (title && title_max) snprintf(title, title_max, "%s", g_meta_title);
        if (title_id && id_max) snprintf(title_id, id_max, "%s", g_meta_id);
        if (version && version_max) snprintf(version, version_max, "%s", g_meta_version);
        if (kind && kind_max) snprintf(kind, kind_max, "%s", g_meta_kind);
    }
    pthread_mutex_unlock(&g_owner_mu);
    return ok ? 0 : -1;
}

int ws_direct_set_icon(const char *owner, const char *sid,
                       const uint8_t *png, size_t size) {
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!owner || !sid || !png || size < sizeof(signature) ||
        size > WS_DIRECT_ICON_MAX || memcmp(png, signature, sizeof(signature)) != 0)
        return -1;
    uint8_t *copy = malloc(size);
    if (!copy) return -1;
    memcpy(copy, png, size);
    pthread_mutex_lock(&g_owner_mu);
    int ok = g_owner[0] && strcmp(owner, g_owner) == 0 &&
             strcmp(sid, g_owner_sid) == 0 && ws_live_check_id(sid);
    if (ok) {
        pthread_mutex_lock(&g_icon_mu);
        free(g_icon_data);
        g_icon_data = copy;
        g_icon_size = size;
        snprintf(g_icon_sid, sizeof(g_icon_sid), "%s", sid);
        pthread_mutex_unlock(&g_icon_mu);
    }
    pthread_mutex_unlock(&g_owner_mu);
    if (!ok) free(copy);
    return ok ? 0 : -1;
}

int ws_direct_get_icon(const char *sid, uint8_t **out_png, size_t *out_size) {
    if (!sid || !out_png || !out_size || !ws_live_check_id(sid)) return -1;
    *out_png = NULL;
    *out_size = 0;
    pthread_mutex_lock(&g_icon_mu);
    int ok = g_icon_data && strcmp(sid, g_icon_sid) == 0;
    if (ok) {
        *out_png = malloc(g_icon_size);
        if (*out_png) {
            memcpy(*out_png, g_icon_data, g_icon_size);
            *out_size = g_icon_size;
        } else {
            ok = 0;
        }
    }
    pthread_mutex_unlock(&g_icon_mu);
    return ok ? 0 : -1;
}

int ws_direct_cancel_owned(const char *owner, const char *sid) {
    if (!owner || !sid) return -1;
    pthread_mutex_lock(&g_owner_mu);
    int ok = g_owner[0] && strcmp(owner, g_owner) == 0 &&
             strcmp(sid, g_owner_sid) == 0 && ws_live_check_id(sid);
    if (!ok) {
        pthread_mutex_unlock(&g_owner_mu);
        return -1;
    }
    /* Cancel the installer as well as the RAM upload. The browser's unload
     * beacon can abort the live session before the WebSocket close arrives;
     * stopping only the session leaves the installer worker running until a
     * later stream error. */
    if (installer_cancel() != 0) ws_direct_cancel_session();
    pthread_mutex_unlock(&g_owner_mu);
    return 0;
}

int ws_direct_write_chunk(uint64_t offset, const void *data, size_t len,
                          uint64_t *out_expected) {
    /* May block on reader backpressure (conn thread: browser just waits). */
    return ws_live_write(offset, data, len, out_expected);
}

/* On success out_uri receives the "live:<id>" URI to hand to
 * POST /api/install (NOT a file path: nothing is stored on disk). */
int ws_direct_finish_session(char *out_uri, size_t uri_max) {
    if (ws_live_finish() != 0) return -1;
    if (out_uri && uri_max) {
        char st[1024] = {0};
        char sid[64] = {0};
        if (ws_live_get_status(st, sizeof(st)) == 0) {
            const char *p = strstr(st, "\"session_id\":\"");
            if (p) {
                p += 14;
                size_t i = 0;
                while (*p && *p != '"' && i + 1 < sizeof(sid)) sid[i++] = *p++;
            }
        }
        snprintf(out_uri, uri_max, "live:%s", sid);
    }
    return 0;
}

void ws_direct_cancel_session(void) {
    pthread_mutex_lock(&g_uplink_mu);
    if (g_uplink_fd >= 0) {
        shutdown(g_uplink_fd, SHUT_RDWR);
        g_uplink_fd = -1;
    }
    pthread_mutex_unlock(&g_uplink_mu);
    ws_live_abort();
    ws_live_destroy();
    clear_icon();
}

void ws_direct_reset_for_tests(void) {
    ws_live_reset_for_tests();
    clear_icon();
    pthread_mutex_lock(&g_owner_mu);
    g_owner[0] = g_owner_sid[0] = '\0';
    pthread_mutex_unlock(&g_owner_mu);
}

int ws_direct_get_status(char *out_json, size_t max) {
    return ws_live_get_status(out_json, max);
}

/* ---------------- SHA1 (public-domain style, compact) ---------------- */

typedef struct { uint32_t h[5]; uint64_t len; unsigned char buf[64]; size_t buflen; } ws_sha1_t;

static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(ws_sha1_t *c, const unsigned char *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t fp, k;
        if (i < 20) { fp = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { fp = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { fp = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { fp = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + fp + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(ws_sha1_t *c) {
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; c->buflen = 0;
}

static void sha1_update(ws_sha1_t *c, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(ws_sha1_t *c, unsigned char out[20]) {
    uint64_t bitlen = c->len * 8;
    unsigned char one = 0x80;
    sha1_update(c, &one, 1);
    unsigned char zero = 0;
    while (c->buflen != 56) sha1_update(c, &zero, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (unsigned char)(bitlen >> (56 - i * 8));
    /* Append length without affecting c->len accounting further. */
    memcpy(c->buf + 56, lb, 8);
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4] = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static int b64_encode(const unsigned char *in, size_t n, char *out, size_t max) {
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((n + 2) / 3) * 4 + 1;
    if (need > max) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i+1] << 8;
        if (i + 2 < n) v |= in[i+2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tab[v & 63] : '=';
    }
    out[o] = '\0';
    return 0;
}

int ws_direct_handshake_accept_key(const char *client_key,
                                   char *out_accept, size_t max) {
    if (!client_key || !client_key[0] || !out_accept || max < 32) return -1;
    /* Trim whitespace (header value). */
    while (*client_key == ' ' || *client_key == '\t') client_key++;
    char key[256];
    snprintf(key, sizeof(key), "%s", client_key);
    size_t klen = strlen(key);
    while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t' ||
                        key[klen-1] == '\r' || key[klen-1] == '\n'))
        key[--klen] = '\0';
    if (klen == 0) return -1;
    char combo[320];
    snprintf(combo, sizeof(combo), "%s%s", key, WS_GUID);
    ws_sha1_t c;
    sha1_init(&c);
    sha1_update(&c, combo, strlen(combo));
    unsigned char digest[20];
    sha1_final(&c, digest);
    return b64_encode(digest, 20, out_accept, max);
}

/* ---------------- frame codec ---------------- */

int ws_direct_frame_encode_server(unsigned char opcode,
                                  const unsigned char *payload, size_t plen,
                                  unsigned char *out, size_t out_max) {
    size_t need = 2 + plen + (plen >= 126 ? (plen > 65535 ? 8 : 2) : 0);
    if (need > out_max) return -1;
    out[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    size_t o = 1;
    if (plen < 126) {
        out[o++] = (unsigned char)plen;
    } else if (plen <= 65535) {
        out[o++] = 126;
        out[o++] = (unsigned char)(plen >> 8);
        out[o++] = (unsigned char)plen;
    } else {
        out[o++] = 127;
        for (int i = 7; i >= 0; i--) out[o++] = (unsigned char)(plen >> (i * 8));
    }
    if (plen && payload) memcpy(out + o, payload, plen);
    return (int)(o + plen);
}

int ws_direct_frame_encode_client(unsigned char opcode,
                                  const unsigned char *payload, size_t plen,
                                  unsigned char *out, size_t out_max) {
    size_t need = 2 + plen + (plen >= 126 ? (plen > 65535 ? 8 : 2) : 0) + 4;
    if (need > out_max) return -1;
    out[0] = (unsigned char)(0x80 | (opcode & 0x0F));
    size_t o = 1;
    if (plen < 126) {
        out[o++] = (unsigned char)(0x80 | plen);
    } else if (plen <= 65535) {
        out[o++] = (unsigned char)(0x80 | 126);
        out[o++] = (unsigned char)(plen >> 8);
        out[o++] = (unsigned char)plen;
    } else {
        out[o++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; i--) out[o++] = (unsigned char)(plen >> (i * 8));
    }
    unsigned char mask[4];
    mask[0] = (unsigned char)rand(); mask[1] = (unsigned char)rand();
    mask[2] = (unsigned char)rand(); mask[3] = (unsigned char)rand();
    memcpy(out + o, mask, 4);
    o += 4;
    for (size_t i = 0; i < plen; i++) out[o + i] = payload[i] ^ mask[i % 4];
    return (int)(o + plen);
}

int ws_direct_frame_decode(unsigned char *buf, size_t buflen,
                           unsigned char *out_opcode, int *out_fin,
                           size_t *out_pay_off, size_t *out_pay_len) {
    if (buflen < 2) return 0;
    unsigned char b0 = buf[0], b1 = buf[1];
    if (b0 & 0x70) return -1; /* RSV1-3 must be 0 */
    unsigned char opcode = b0 & 0x0F;
    if (opcode > 0x0A || (opcode > 0x02 && opcode < 0x08)) return -1;
    int fin = (b0 & 0x80) ? 1 : 0;
    int masked = (b1 & 0x80) ? 1 : 0;
    uint64_t plen = b1 & 0x7F;
    size_t hdr = 2;
    if (plen == 126) {
        if (buflen < 4) return 0;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        hdr = 4;
    } else if (plen == 127) {
        if (buflen < 10) return 0;
        if (buf[2] & 0x80) return -1; /* length MSB must be 0 */
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | buf[2 + i];
        hdr = 10;
    }
    if (opcode >= 0x08) {
        if (!fin || plen > 125) return -1; /* control frames: FIN + <=125 */
    }
    size_t masklen = masked ? 4 : 0;
    if (plen > (uint64_t)(SIZE_MAX - hdr - masklen)) return -1;
    if (buflen < hdr + masklen + (size_t)plen) return 0; /* need more */
    if (masked) {
        unsigned char *mask = buf + hdr;
        unsigned char *pay = buf + hdr + 4;
        for (uint64_t i = 0; i < plen; i++) pay[i] ^= mask[i % 4];
    }
    if (out_opcode) *out_opcode = opcode;
    if (out_fin) *out_fin = fin;
    if (out_pay_off) *out_pay_off = hdr + masklen;
    if (out_pay_len) *out_pay_len = (size_t)plen;
    return (int)(hdr + masklen + (size_t)plen);
}

/* ---------------- listener ---------------- */

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Minimal JSON extractors for control messages (no deps). */
static int wsj_string(const char *json, const char *key, char *out, size_t max) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p = strchr(p, '"');
    if (!p) return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < max) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int wsj_u64(const char *json, const char *key, uint64_t *out) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') p++;
    *out = strtoull(p, NULL, 10);
    return 0;
}

static int send_text(int fd, const char *s) {
    unsigned char fr[WS_TEXT_MAX + 16];
    int n = ws_direct_frame_encode_server(0x1, (const unsigned char *)s,
                                          strlen(s), fr, sizeof(fr));
    if (n < 0) return -1;
    return send_all(fd, fr, (size_t)n);
}

/* Serialized control send (acks, errors, seeks share one socket). */
static int send_text_locked(int fd, const char *s) {
    pthread_mutex_lock(&g_uplink_mu);
    int r = send_text(fd, s);
    pthread_mutex_unlock(&g_uplink_mu);
    return r;
}

/* Seek sink for blocked readers (registered with ws_live). */
static void uplink_seek(uint64_t seg) {
    char msg[96];
    snprintf(msg, sizeof(msg), "{\"op\":\"seek\",\"seg\":%llu}",
             (unsigned long long)seg);
    pthread_mutex_lock(&g_uplink_mu);
    int fd = g_uplink_fd;
    if (fd >= 0) {
        /* Raw send: the lock is already held (locked wrapper would
         * self-deadlock on this non-recursive mutex). */
        if (send_text(fd, msg) != 0) {
            install_log("[WS] ERROR: seek send failed: %s", strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_uplink_mu);
}

static void handle_text_msg(int fd, const char *msg, long *pending_seg,
                            int *authorized, char *authorized_sid,
                            int *finish_received) {
    char op[32] = {0};
    if (wsj_string(msg, "op", op, sizeof(op)) != 0) {
        send_text_locked(fd, "{\"op\":\"error\",\"error\":\"missing op\"}");
        return;
    }
    if (strcmp(op, "init") == 0) {
        char fn[WS_DIRECT_FILENAME_MAX] = {0};
        char owner[65] = {0}, requested_sid[64] = {0};
        uint64_t total = 0;
        if (wsj_string(msg, "filename", fn, sizeof(fn)) != 0 ||
            wsj_u64(msg, "total", &total) != 0 || total == 0) {
            send_text_locked(fd, "{\"op\":\"error\",\"error\":\"init needs filename+total\"}");
            return;
        }
        char sid[WS_DIRECT_SESSION_ID_MAX] = {0};
        int rc;
        pthread_mutex_lock(&g_owner_mu);
        int owned = g_owner[0] != '\0';
        pthread_mutex_unlock(&g_owner_mu);
        if (owned) {
            if (wsj_string(msg, "owner", owner, sizeof(owner)) != 0 ||
                wsj_string(msg, "session_id", requested_sid, sizeof(requested_sid)) != 0 ||
                !ws_direct_owner_matches(owner, requested_sid)) {
                send_text_locked(fd, "{\"op\":\"error\",\"error\":\"Another upload is active\"}");
                return;
            }
            pthread_mutex_lock(&g_owner_mu);
            int same_file = strcmp(fn, g_owner_filename) == 0 && total == g_owner_total;
            pthread_mutex_unlock(&g_owner_mu);
            if (!same_file) {
                send_text_locked(fd, "{\"op\":\"error\",\"error\":\"File does not match session\"}");
                return;
            }
            snprintf(sid, sizeof(sid), "%s", requested_sid);
            rc = 0;
        } else {
            rc = ws_direct_init_session(fn, total, sid, sizeof(sid));
        }
        if (rc == -2) {
            send_text_locked(fd, "{\"op\":\"error\",\"error\":\"busy\"}");
            return;
        }
        if (rc != 0) {
            send_text_locked(fd, "{\"op\":\"error\",\"error\":\"init failed\"}");
            return;
        }
        pthread_mutex_lock(&g_uplink_mu);
        if (g_uplink_fd >= 0 && g_uplink_fd != fd) {
            pthread_mutex_unlock(&g_uplink_mu);
            send_text_locked(fd, "{\"op\":\"error\",\"error\":\"Uploader already connected\"}");
            return;
        }
        g_uplink_fd = fd;
        pthread_mutex_unlock(&g_uplink_mu);
        *authorized = 1;
        snprintf(authorized_sid, WS_DIRECT_SESSION_ID_MAX, "%s", sid);
        uint64_t r = ws_live_get_resume_offset();
        char rep[320];
        snprintf(rep, sizeof(rep),
                 "{\"op\":\"ready\",\"session_id\":\"%s\",\"offset\":%llu}",
                 sid, (unsigned long long)r);
        send_text_locked(fd, rep);
    } else if (strcmp(op, "status") == 0) {
        char st[768];
        ws_direct_get_status(st, sizeof(st));
        send_text_locked(fd, st);
    } else if (strcmp(op, "finish") == 0) {
        if (!*authorized || !ws_live_check_id(authorized_sid)) { send_text_locked(fd, "{\"op\":\"error\",\"error\":\"Unauthorized\"}"); return; }
        char path[640] = {0};
        if (ws_direct_finish_session(path, sizeof(path)) != 0) {
            char rep[256];
            uint64_t r = 0, t = 0;
            ws_live_get_counters(&r, &t);
            snprintf(rep, sizeof(rep),
                     "{\"op\":\"error\",\"error\":\"incomplete\",\"received\":%llu,\"total\":%llu}",
                     (unsigned long long)r, (unsigned long long)t);
            send_text_locked(fd, rep);
            return;
        }
        char rep[768];
        snprintf(rep, sizeof(rep),
                 "{\"op\":\"complete\",\"path\":\"%s\"}", path);
        *finish_received = 1;
        send_text_locked(fd, rep);
    } else if (strcmp(op, "cancel") == 0) {
        if (!*authorized || !ws_live_check_id(authorized_sid)) { send_text_locked(fd, "{\"op\":\"error\",\"error\":\"Unauthorized\"}"); return; }
        ws_direct_cancel_session();
        send_text_locked(fd, "{\"op\":\"cancelled\"}");
    } else if (strcmp(op, "seg") == 0) {
        if (!*authorized || !ws_live_check_id(authorized_sid)) { send_text_locked(fd, "{\"op\":\"error\",\"error\":\"Unauthorized\"}"); return; }
        /* Address the following binary message at segment S. Out-of-range
         * segments fail at apply time; just record it. */
        uint64_t seg = 0;
        const char *sp = strstr(msg, "\"seg\":");
        if (sp) seg = strtoull(sp + 6, NULL, 10);
        *pending_seg = (seg <= 0x7fffffffUL) ? (long)seg : -1;
    } else {
        send_text_locked(fd, "{\"op\":\"error\",\"error\":\"unknown op\"}");
    }
}

static int has_header(const char *hdrs, const char *name, char *val, size_t vmax) {
    size_t nlen = strlen(name);
    const char *p = hdrs;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nlen + 1) {
            size_t i;
            int match = 1;
            for (i = 0; i < nlen; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { match = 0; break; }
            }
            if (match && p[nlen] == ':') {
                const char *v = p + nlen + 1;
                while (v < eol && (*v == ' ' || *v == '\t')) v++;
                size_t vl = (size_t)(eol - v);
                if (vl >= vmax) vl = vmax - 1;
                memcpy(val, v, vl);
                val[vl] = '\0';
                while (vl > 0 && (val[vl-1] == ' ' || val[vl-1] == '\t' ||
                                 val[vl-1] == '\r' || val[vl-1] == '\n'))
                    val[--vl] = '\0';
                return 1;
            }
        }
        if (*eol == '\0') break;
        p = (*eol == '\r' && *(eol + 1) == '\n') ? eol + 2 : eol + 1;
        if (p[0] == '\r' || p[0] == '\n' || p[0] == '\0') break;
    }
    return 0;
}

static void *ws_conn_worker(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    live_add(fd);

    struct timeval tv = { .tv_sec = WS_IO_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Read HTTP handshake headers. */
    char hs[8192];
    size_t hlen = 0;
    hs[0] = '\0';
    while (hlen < sizeof(hs) - 1) {
        ssize_t n = recv(fd, hs + hlen, sizeof(hs) - 1 - hlen, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        hlen += (size_t)n;
        hs[hlen] = '\0';
        if (strstr(hs, "\r\n\r\n")) break;
    }
    char *hend = strstr(hs, "\r\n\r\n");
    size_t keep = 0; /* bytes after headers belonging to frame stream */
    if (hend) keep = hlen - (size_t)(hend + 4 - hs);

    char method[16] = {0}, path[512] = {0};
    sscanf(hs, "%15s %511s", method, path);
    char up[64] = {0}, key[256] = {0};
    int ok_upgrade = has_header(hs, "Upgrade", up, sizeof(up)) &&
                     strcasestr(up, "websocket") != NULL;
    int ok_key = has_header(hs, "Sec-WebSocket-Key", key, sizeof(key));
    int ok_path = (strncmp(path, "/ws/upload", 10) == 0);

    if (!hend || strcmp(method, "GET") != 0 || !ok_path || !ok_upgrade || !ok_key) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n";
        send_all(fd, bad, strlen(bad));
        install_log("[WS] handshake rejected: method='%.15s' path='%.100s' "
                    "upgrade=%d key=%d hdr_bytes=%zu",
                    method, path, ok_upgrade, ok_key, hlen);
        live_remove(fd);
        close(fd);
        return NULL;
    }
    char accept[64] = {0};
    if (ws_direct_handshake_accept_key(key, accept, sizeof(accept)) != 0) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
                          "Connection: close\r\n\r\n";
        send_all(fd, bad, strlen(bad));
        live_remove(fd);
        close(fd);
        return NULL;
    }
    char rep[512];
    int rlen = snprintf(rep, sizeof(rep),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (rlen <= 0 || send_all(fd, rep, (size_t)rlen) != 0) {
        install_log("[WS] ERROR: failed to send 101 handshake: %s", strerror(errno));
        live_remove(fd);
        close(fd);
        return NULL;
    }
    install_log("[WS] connection upgraded: path='%.100s' pipelined=%zu", path, keep);

    /* Frame loop with reassembly buffers (heap: 8 MiB never on thread stack). */
    unsigned char *bin_msg = NULL;
    size_t bin_cap = 0;
    long pending_seg = -1;
    int authorized = 0;
    int finish_received = 0;
    int was_uplink = 0;
    char authorized_sid[WS_DIRECT_SESSION_ID_MAX] = {0};
    unsigned char *rbuf = (unsigned char *)malloc(WS_BIN_MSG_MAX + 1024);
    if (!rbuf) { live_remove(fd); close(fd); return NULL; }
    size_t rcap = WS_BIN_MSG_MAX + 1024;
    size_t rlen2 = 0;
    if (keep > 0) {
        if (keep > rcap) keep = 0; /* cannot happen; stay safe */
        else { memmove(rbuf, hend + 4, keep); rlen2 = keep; }
    }
    char frag_text[WS_TEXT_MAX + 1];
    size_t frag_tlen = 0;
    int frag_text_on = 0;
    unsigned char frag_op = 0;
    /* Binary message reassembly: every binary message belongs to the
     * segment named by the preceding {"op":"seg","seg":S} text message
     * (explicit addressing: the browser seeks freely). The full message is
     * buffered, then applied atomically at FIN. */
    size_t frag_bin_total = 0;
    unsigned long n_text = 0, n_bin = 0, n_acks = 0, n_busy = 0;
    uint64_t bin_bytes = 0;

    for (;;) {
        /* Drain complete frames already buffered BEFORE blocking in recv:
         * the peer is often waiting for our reply (e.g. ack) and will send
         * nothing more, so recving first deadlocks both sides. */
        while (rlen2 > 0) {
            unsigned char opcode = 0;
            int fin = 0;
            size_t poff = 0, plen = 0;
            int used = ws_direct_frame_decode(rbuf, rlen2, &opcode, &fin,
                                              &poff, &plen);
            if (used == 0) {
                if (rlen2 >= rcap) {
                    install_log("[WS] ERROR: message exceeds %u bytes, closing",
                                WS_BIN_MSG_MAX);
                    goto conn_done;
                }
                break; /* partial frame: leave inner loop and recv more */
            }
            if (used < 0) {
                install_log("[WS] ERROR: frame decode failed (buf=%zu), closing", rlen2);
                goto conn_done;
            }
        unsigned char *pay = rbuf + poff;
        if (opcode == 0x8) { /* close: echo and exit */
            unsigned char fr[16];
            int m = ws_direct_frame_encode_server(0x8, pay, plen > 125 ? 0 : plen,
                                                  fr, sizeof(fr));
            if (m > 0) send_all(fd, fr, (size_t)m);
            goto conn_done;
        } else if (opcode == 0x9) { /* ping -> pong */
            unsigned char fr[256];
            int m = ws_direct_frame_encode_server(0xA, pay, plen > 125 ? 0 : plen,
                                                  fr, sizeof(fr));
            if (m > 0) send_all(fd, fr, (size_t)m);
        } else if (opcode == 0xA) {
            /* pong: ignore */
        } else if (opcode == 0x1 || (opcode == 0x0 && frag_op == 0x1)) {
            int is_start = (opcode == 0x1);
            if (is_start && frag_text_on) {
                install_log("[WS] ERROR: interleaved text frame, closing");
                goto conn_done; /* interleaved */
            }
            if (!is_start && !frag_text_on) {
                install_log("[WS] ERROR: stray text continuation, closing");
                goto conn_done;
            }
            if (is_start) { frag_tlen = 0; frag_op = 0x1; frag_text_on = 1; }
            if (frag_tlen + plen > WS_TEXT_MAX) {
                install_log("[WS] ERROR: text message exceeds %u bytes, closing",
                            WS_TEXT_MAX);
                goto conn_done;
            }
            memcpy(frag_text + frag_tlen, pay, plen);
            frag_tlen += plen;
            if (fin) {
                frag_text[frag_tlen] = '\0';
                n_text++;
                handle_text_msg(fd, frag_text, &pending_seg, &authorized, authorized_sid,
                                &finish_received);
                frag_text_on = 0;
                frag_tlen = 0;
            }
        } else if (opcode == 0x2 || (opcode == 0x0 && frag_op == 0x2)) {
            int is_start = (opcode == 0x2);
            if (!is_start && !frag_text_on) {
                install_log("[WS] ERROR: stray binary continuation, closing");
                goto conn_done;
            }
            if (is_start) {
                frag_op = 0x2;
                frag_text_on = 1;
                frag_bin_total = 0;
                if (pending_seg < 0) {
                    install_log("[WS] ERROR: binary without segment header, closing");
                    goto conn_done;
                }
            }
            if (frag_bin_total + plen > WS_BIN_MSG_MAX) {
                install_log("[WS] ERROR: binary message exceeds %u bytes, closing",
                            WS_BIN_MSG_MAX);
                goto conn_done;
            }
            if (frag_bin_total + plen > bin_cap) {
                size_t ncap = frag_bin_total + plen;
                if (ncap < 65536) ncap = 65536;
                unsigned char *nb = realloc(bin_msg, ncap);
                if (!nb) {
                    install_log("[WS] ERROR: binary buffer alloc failed, closing");
                    goto conn_done;
                }
                bin_msg = nb;
                bin_cap = ncap;
            }
            memcpy(bin_msg + frag_bin_total, pay, plen);
            frag_bin_total += plen;
            if (!fin) {
                /* More fragments coming: consume this frame and wait. */
            } else {
            /* Whole message reassembled: apply to the named segment. */
            {
                uint64_t seg = (uint64_t)pending_seg;
                uint64_t total = ws_live_get_total();
                uint64_t segs = total ? (total + WS_LIVE_SEG_SIZE - 1) / WS_LIVE_SEG_SIZE : 0;
                uint64_t exp_len = WS_LIVE_SEG_SIZE;
                if (seg < segs && seg == segs - 1) exp_len = total - seg * WS_LIVE_SEG_SIZE;
                if (!authorized || !ws_live_check_id(authorized_sid) ||
                    !ws_direct_session_active() || seg >= segs ||
                    frag_bin_total != (size_t)exp_len) {
                    install_log("[WS] binary message rejected (seg=%llu len=%zu session=%d)",
                                (unsigned long long)seg, frag_bin_total,
                                ws_direct_session_active());
                    send_text_locked(fd, "{\"op\":\"error\",\"error\":\"bad segment\"}");
                } else {
                    /* Never block the worker on storage: an unstoreable
                     * segment gets "busy" (browser requeues + retries after
                     * 50 ms) so the socket keeps flowing and parked readers
                     * keep being served. Only a truly bad message is fatal.
                     * Waited-on writes practically always land at once
                     * (ws_live_try_write documents why). */
                    int wr = ws_live_try_write(seg * WS_LIVE_SEG_SIZE,
                                               bin_msg, frag_bin_total);
                    if (wr == 0) {
                        bin_bytes += frag_bin_total;
                        n_bin++;
                        char am[160];
                        snprintf(am, sizeof(am),
                                 "{\"op\":\"ack\",\"seg\":%llu}",
                                 (unsigned long long)seg);
                        if (send_text_locked(fd, am) != 0) {
                            install_log("[WS] ERROR: ack send failed: %s",
                                        strerror(errno));
                            goto conn_done;
                        }
                        n_acks++;
                    } else if (wr == -2) {
                        /* Throttled: a sustained busy storm means the
                         * producer outruns the readers (sequential
                         * overflow); the browser's 50 ms retry absorbs it. */
                        n_busy++;
                        char bm[160];
                        snprintf(bm, sizeof(bm),
                                 "{\"op\":\"busy\",\"seg\":%llu}",
                                 (unsigned long long)seg);
                        send_text_locked(fd, bm);
                    } else {
                        install_log("[WS] chunk rejected (seg=%llu len=%zu wr=%d)",
                                    (unsigned long long)seg, frag_bin_total, wr);
                        send_text_locked(fd, "{\"op\":\"error\",\"error\":\"rejected\"}");
                    }
                }
                pending_seg = -1;
                frag_text_on = 0;
                frag_bin_total = 0;
            } /* end if (fin): message applied */
            } /* end else: accumulating fragments */
            if (fin) { frag_text_on = 0; }
        } else {
            install_log("[WS] ERROR: bad frame (op=%u frag_op=%u), closing",
                        opcode, frag_op);
            goto conn_done; /* unknown data opcode state */
        }
        memmove(rbuf, rbuf + used, rlen2 - (size_t)used);
        rlen2 -= (size_t)used;
        } /* end drain loop */
        /* Buffer holds less than one full frame: wait for more bytes. */
        if (rlen2 >= rcap) {
            install_log("[WS] ERROR: message exceeds %u bytes, closing",
                        WS_BIN_MSG_MAX);
            goto conn_done;
        }
        {
            ssize_t n = recv(fd, rbuf + rlen2, rcap - rlen2, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Virtual block device: while installer readers are
                     * parked on seeks, a silent browser (baseline done,
                     * resend not yet due) must not cost the socket. Reader
                     * timeouts/abort still drain the waiter count, so this
                     * cannot hang forever: once nobody waits, the next
                     * idle tick closes as before. */
                    int parked = ws_live_waiter_count();
                    if (parked > 0) {
                        continue;
                    }
                    install_log("[WS] conn idle timeout after %lu text/%lu bin frames (%llu bytes)",
                                n_text, n_bin, (unsigned long long)bin_bytes);
                } else {
                    install_log("[WS] conn recv error: %s", strerror(errno));
                }
                goto conn_done;
            }
            if (n == 0) goto conn_done; /* peer closed */
            rlen2 += (size_t)n;
        }
    }
conn_done:
    pthread_mutex_lock(&g_uplink_mu);
    if (g_uplink_fd == fd) {
        was_uplink = 1;
        g_uplink_fd = -1;
    }
    pthread_mutex_unlock(&g_uplink_mu);
    if (was_uplink && authorized && !finish_received) {
        install_log("[WS] uploader disconnected before completion; canceling install");
        if (installer_cancel() != 0 && ws_direct_session_active()) ws_direct_cancel_session();
    }
    free(bin_msg);
    free(rbuf);
    install_log("[WS] connection closed: %lu text/%lu bin frames, %llu bytes, %lu acks, %lu busy",
                n_text, n_bin, (unsigned long long)bin_bytes, n_acks, n_busy);
    live_remove(fd);
    close(fd);
    return NULL;
}

static void *ws_listener_fn(void *arg) {
    (void)arg;
    while (g_listen_running) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int c = accept(g_listen_fd, (struct sockaddr *)&cli, &cl);
        if (c < 0) {
            if (errno == EINTR) continue;
            if (!g_listen_running) break;
            usleep(10000);
            continue;
        }
        int *pfd = (int *)malloc(sizeof(int));
        if (!pfd) { close(c); continue; }
        *pfd = c;
        pthread_t th;
        if (pthread_create(&th, NULL, ws_conn_worker, pfd) == 0)
            pthread_detach(th);
        else { close(c); free(pfd); }
    }
    return NULL;
}

int ws_direct_listener_start(int port) {
    if (g_listen_running) return 0;
    if (port <= 0) port = WS_DIRECT_DEFAULT_PORT;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        install_log("[WS] ERROR: bind :%d failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        install_log("[WS] ERROR: listen :%d failed: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    g_listen_fd = fd;
    g_listen_port = port;
    g_listen_running = 1;
    live_init();
    if (pthread_create(&g_listen_thread, NULL, ws_listener_fn, NULL) != 0) {
        g_listen_running = 0;
        close(fd);
        g_listen_fd = -1;
        return -1;
    }
    g_listen_created = 1;
    ws_live_set_seek_fn(uplink_seek);
    install_log("[WS] upload listener on 0.0.0.0:%d", port);
    return 0;
}

int ws_direct_ensure_listener(void) {
    if (g_listen_running) return 0;
    const char *env = getenv("WS_DIRECT_PORT");
    int port = WS_DIRECT_DEFAULT_PORT;
    if (env && env[0]) {
        long v = strtol(env, NULL, 10);
        if (v > 0 && v < 65536) port = (int)v;
    }
    /* Lazy best-effort: a missing listener must not fail the spool path. */
    if (ws_direct_listener_start(port) != 0) return -1;
    return 0;
}

void ws_direct_listener_stop(void) {
    int join = 0, lfd = -1;
    int kick[WS_MAX_TRACKED], nk = 0;
    live_init();
    pthread_mutex_lock(&g_ws_mu);
    if (g_listen_running || g_listen_created) {
        g_listen_running = 0;
        join = g_listen_created;
        g_listen_created = 0;
    }
    lfd = g_listen_fd;
    g_listen_fd = -1;
    for (int i = 0; i < WS_MAX_TRACKED && nk < WS_MAX_TRACKED; i++) {
        if (g_live[i] >= 0) { kick[nk++] = g_live[i]; g_live[i] = -1; }
    }
    pthread_mutex_unlock(&g_ws_mu);
    for (int i = 0; i < nk; i++) shutdown(kick[i], SHUT_RDWR);
    if (lfd >= 0) { shutdown(lfd, SHUT_RDWR); close(lfd); }
    if (join) pthread_join(g_listen_thread, NULL);
    g_listen_port = 0;
}

int ws_direct_listener_running(void) {
    return g_listen_running;
}

int ws_direct_listener_port(void) {
    return g_listen_port;
}
