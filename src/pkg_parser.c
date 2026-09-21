/*
 * PKG Manager - Binary Package & Metadata Parser
 *
 * Reads PS4 (CNT) and PS5 (FIH) package headers, parses param.sfo /
 * param.json tables, and extracts embedded icon0.png artwork.
 */

#include "pkg_parser.h"
#include "multipart.h"
#include "smb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

/* Endian helpers */
static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t read_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (i * 8);
    }
    return v;
}

/* Helper to extract a string value for a given key from JSON */
static int json_extract_key(const char *json, size_t json_len, const char *key, char *out, size_t out_max) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = json;
    const char *end = json + json_len;

    while (p < end) {
        const char *found = strstr(p, pattern);
        if (!found || found >= end) {
            return -1;
        }

        const char *colon = strchr(found + strlen(pattern), ':');
        if (!colon || colon >= end) {
            return -1;
        }

        const char *q = colon + 1;
        while (q < end && isspace((unsigned char)*q)) {
            q++;
        }

        if (q < end && *q == '"') {
            q++;
            size_t idx = 0;
            while (q < end && *q != '"') {
                if (*q == '\\' && (q + 1) < end) {
                    q++;
                }
                if (idx + 1 < out_max) {
                    out[idx++] = *q;
                }
                q++;
            }
            out[idx] = '\0';
            return 0;
        }

        /* If value was not a string (e.g. nested object or null), keep searching next occurrence */
        p = found + strlen(pattern);
    }

    return -1;
}

/* Length-bounded key presence check for numeric and string JSON fields. */
static int json_has_key(const char *json, size_t json_len, const char *key) {
    char pattern[128];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key ? key : "");
    if (!json || !key || n <= 0 || (size_t)n >= sizeof(pattern)) return 0;
    for (size_t i = 0; i + (size_t)n <= json_len; i++) {
        if (memcmp(json + i, pattern, (size_t)n) == 0) return 1;
    }
    return 0;
}

/* Helper to extract value string for a specific key from a flat JSON object {"k":"v", ...} */
static int extract_val_for_key(const char *json, const char *key, char *out, size_t out_max) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    size_t idx = 0;
    while (*p && *p != '"' && idx + 1 < out_max) {
        if (*p == '\\' && *(p + 1)) p++;
        out[idx++] = *p++;
    }
    out[idx] = '\0';
    return 0;
}

int pkg_parser_resolve_localized_title(const char *loc_json, const char *default_lang,
                                       const char *accept_lang, char *out, size_t out_max) {
    if (!loc_json || loc_json[0] != '{' || !out || out_max == 0) return -1;
    out[0] = '\0';

    if (accept_lang && accept_lang[0]) {
        const char *p = accept_lang;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            const char *item_start = p;
            while (*p && *p != ',' && *p != ';') p++;
            size_t tag_len = p - item_start;
            while (tag_len > 0 && isspace((unsigned char)item_start[tag_len - 1])) tag_len--;

            if (tag_len > 0 && tag_len < 32) {
                char tag[32];
                memcpy(tag, item_start, tag_len);
                tag[tag_len] = '\0';

                /* 1. Exact match */
                if (extract_val_for_key(loc_json, tag, out, out_max) == 0 && out[0]) {
                    return 0;
                }

                /* 2. Prefix match: e.g. "en" matches "en-US", or "en-GB" matches "en" */
                char primary[16] = {0};
                const char *dash = strchr(tag, '-');
                if (!dash) dash = strchr(tag, '_');
                size_t plen = dash ? (size_t)(dash - tag) : tag_len;
                if (plen < sizeof(primary)) {
                    memcpy(primary, tag, plen);
                    primary[plen] = '\0';

                    char pat1[32], pat2[32];
                    snprintf(pat1, sizeof(pat1), "\"%s-", primary);
                    snprintf(pat2, sizeof(pat2), "\"%s\":", primary);

                    const char *found = strstr(loc_json, pat1);
                    if (!found) found = strstr(loc_json, pat2);
                    if (found) {
                        found++; /* skip opening quote */
                        const char *end_key = strchr(found, '"');
                        if (end_key) {
                            size_t klen = end_key - found;
                            char matched_key[32];
                            if (klen < sizeof(matched_key)) {
                                memcpy(matched_key, found, klen);
                                matched_key[klen] = '\0';
                                if (extract_val_for_key(loc_json, matched_key, out, out_max) == 0 && out[0]) {
                                    return 0;
                                }
                            }
                        }
                    }
                }
            }

            /* Skip any ;q=... quality values until next comma */
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }

    /* Fallback 1: default_lang */
    if (default_lang && default_lang[0]) {
        if (extract_val_for_key(loc_json, default_lang, out, out_max) == 0 && out[0]) {
            return 0;
        }
        char def_primary[16] = {0};
        const char *dash = strchr(default_lang, '-');
        if (!dash) dash = strchr(default_lang, '_');
        size_t plen = dash ? (size_t)(dash - default_lang) : strlen(default_lang);
        if (plen < sizeof(def_primary)) {
            memcpy(def_primary, default_lang, plen);
            def_primary[plen] = '\0';
            char pat1[32];
            snprintf(pat1, sizeof(pat1), "\"%s-", def_primary);
            const char *found = strstr(loc_json, pat1);
            if (found) {
                found++;
                const char *end_k = strchr(found, '"');
                if (end_k) {
                    size_t kl = end_k - found;
                    char mk[32];
                    if (kl < sizeof(mk)) {
                        memcpy(mk, found, kl);
                        mk[kl] = '\0';
                        if (extract_val_for_key(loc_json, mk, out, out_max) == 0 && out[0]) {
                            return 0;
                        }
                    }
                }
            }
        }
    }

    /* Fallback 2: English */
    const char *en_match = strstr(loc_json, "\"en-");
    if (!en_match) en_match = strstr(loc_json, "\"en\":");
    if (en_match) {
        en_match++;
        const char *eq = strchr(en_match, '"');
        if (eq) {
            char k[32];
            size_t l = eq - en_match;
            if (l < sizeof(k)) {
                memcpy(k, en_match, l);
                k[l] = '\0';
                if (extract_val_for_key(loc_json, k, out, out_max) == 0 && out[0]) {
                    return 0;
                }
            }
        }
    }

    /* Fallback 3: first value in loc_json */
    const char *colon = strchr(loc_json, ':');
    if (colon) {
        if (*(colon + 1) == '"') {
            const char *vs = colon + 2;
            const char *ve = strchr(vs, '"');
            if (ve) {
                size_t l = ve - vs;
                if (l + 1 < out_max) {
                    memcpy(out, vs, l);
                    out[l] = '\0';
                    return 0;
                }
            }
        }
    }

    return -1;
}

void pkg_parser_parse_param_json(const char *json_buf, size_t data_sz,
                                 char *out_title_id, size_t tid_max,
                                 char *out_title_name, size_t tname_max,
                                 char *out_category, size_t cat_max,
                                 char *out_version, size_t ver_max,
                                 char *out_localized_titles, size_t loc_max,
                                 char *out_default_lang, size_t def_lang_max) {
    if (!json_buf || data_sz == 0) return;
    const char *end = json_buf + data_sz;

    if (out_title_id && tid_max > 0 && out_title_id[0] == '\0') {
        char tid[PKG_TITLE_ID_LEN] = {0};
        if (json_extract_key(json_buf, data_sz, "titleId", tid, sizeof(tid)) == 0) {
            strncpy(out_title_id, tid, tid_max - 1);
            out_title_id[tid_max - 1] = '\0';
        }
    }

    if (out_category && cat_max > 0 && out_category[0] == '\0') {
        char cat_buf[16] = {0};
        if (json_extract_key(json_buf, data_sz, "category", cat_buf, sizeof(cat_buf)) == 0) {
            strncpy(out_category, cat_buf, cat_max - 1);
            out_category[cat_max - 1] = '\0';
        }
    }

    if (out_version && ver_max > 0 && out_version[0] == '\0') {
        char ver[32] = {0};
        if (json_extract_key(json_buf, data_sz, "contentVersion", ver, sizeof(ver)) == 0 ||
            json_extract_key(json_buf, data_sz, "appVersion", ver, sizeof(ver)) == 0 ||
            json_extract_key(json_buf, data_sz, "version", ver, sizeof(ver)) == 0) {
            if (ver[0] != '\0') {
                int maj = 0, min = 0, patch = 0;
                if (sscanf(ver, "%d.%d.%d", &maj, &min, &patch) == 3) {
                    if (min == 0 && patch > 0) {
                        snprintf(out_version, ver_max, "v%d.%02d", maj, patch);
                    } else if (patch == 0) {
                        snprintf(out_version, ver_max, "v%d.%02d", maj, min);
                    } else {
                        snprintf(out_version, ver_max, "v%d.%d.%d", maj, min, patch);
                    }
                } else if (ver[0] != 'v' && ver[0] != 'V') {
                    snprintf(out_version, ver_max, "v%.29s", ver);
                } else {
                    strncpy(out_version, ver, ver_max - 1);
                    out_version[ver_max - 1] = '\0';
                }
            }
        }
    }

    /* Check for localizedParameters block */
    const char *lp = strstr(json_buf, "\"localizedParameters\"");
    const char *lp_start = NULL;
    const char *lp_end = NULL;

    if (lp && lp < end) {
        const char *colon = strchr(lp + 21, ':');
        if (colon && colon < end) {
            const char *brace = strchr(colon + 1, '{');
            if (brace && brace < end) {
                int depth = 0;
                const char *p = brace;
                int in_str = 0;
                while (p < end) {
                    if (*p == '\\' && in_str && (p + 1) < end) {
                        p += 2;
                        continue;
                    }
                    if (*p == '"') in_str = !in_str;
                    else if (!in_str) {
                        if (*p == '{') depth++;
                        else if (*p == '}') {
                            depth--;
                            if (depth == 0) {
                                lp_start = brace;
                                lp_end = p;
                                break;
                            }
                        }
                    }
                    p++;
                }
            }
        }
    }

    /* Check if there is a root / global title outside localizedParameters */
    char global_title[PKG_TITLE_NAME_LEN] = {0};
    const char *cand_patterns[] = {"\"titleName\"", "\"title\""};
    for (int cp = 0; cp < 2 && global_title[0] == '\0'; cp++) {
        const char *curr = json_buf;
        while (curr < end) {
            const char *f = strstr(curr, cand_patterns[cp]);
            if (!f || f >= end) break;
            /* If this occurrence is outside localizedParameters */
            if (!lp_start || f < lp_start || f > lp_end) {
                const char *c = strchr(f + strlen(cand_patterns[cp]), ':');
                if (c && c < end) {
                    const char *q = c + 1;
                    while (q < end && isspace((unsigned char)*q)) q++;
                    if (q < end && *q == '"') {
                        q++;
                        size_t gidx = 0;
                        while (q < end && *q != '"') {
                            if (*q == '\\' && (q + 1) < end) q++;
                            if (gidx + 1 < sizeof(global_title)) global_title[gidx++] = *q;
                            q++;
                        }
                        global_title[gidx] = '\0';
                        break;
                    }
                }
            }
            curr = f + strlen(cand_patterns[cp]);
        }
    }

    if (out_default_lang && def_lang_max > 0) {
        out_default_lang[0] = '\0';
    }
    if (out_localized_titles && loc_max > 0) {
        out_localized_titles[0] = '\0';
    }

    /* If localizedParameters exists, parse it */
    if (lp_start && lp_end) {
        /* Extract defaultLanguage */
        char def_lang[32] = {0};
        const char *dl = strstr(lp_start, "\"defaultLanguage\"");
        if (dl && dl < lp_end) {
            const char *dl_colon = strchr(dl + 17, ':');
            if (dl_colon && dl_colon < lp_end) {
                const char *q = dl_colon + 1;
                while (q < lp_end && isspace((unsigned char)*q)) q++;
                if (q < lp_end && *q == '"') {
                    q++;
                    size_t didx = 0;
                    while (q < lp_end && *q != '"') {
                        if (*q == '\\' && (q + 1) < lp_end) q++;
                        if (didx + 1 < sizeof(def_lang)) def_lang[didx++] = *q;
                        q++;
                    }
                    def_lang[didx] = '\0';
                }
            }
        }
        if (out_default_lang && def_lang_max > 0 && def_lang[0]) {
            strncpy(out_default_lang, def_lang, def_lang_max - 1);
            out_default_lang[def_lang_max - 1] = '\0';
        }

        /* Iterate language entries in localizedParameters */
        struct {
            char lang[32];
            char title[PKG_TITLE_NAME_LEN];
        } entries[64];
        size_t count = 0;

        const char *p = lp_start + 1;
        while (p < lp_end && count < 64) {
            while (p < lp_end && *p != '"') p++;
            if (p >= lp_end) break;

            char key[64] = {0};
            p++;
            size_t kidx = 0;
            while (p < lp_end && *p != '"') {
                if (kidx + 1 < sizeof(key)) key[kidx++] = *p;
                p++;
            }
            if (p >= lp_end) break;
            p++;

            while (p < lp_end && isspace((unsigned char)*p)) p++;
            if (p >= lp_end || *p != ':') continue;
            p++;

            while (p < lp_end && isspace((unsigned char)*p)) p++;
            if (p >= lp_end) break;

            if (*p == '"') {
                p++;
                while (p < lp_end && *p != '"') {
                    if (*p == '\\' && (p + 1) < lp_end) p++;
                    p++;
                }
                if (p < lp_end) p++;
                continue;
            }

            if (*p == '{') {
                const char *obj_start = p;
                int obj_depth = 0;
                const char *obj_end = NULL;
                int obj_in_str = 0;
                while (p < lp_end) {
                    if (*p == '\\' && obj_in_str && (p + 1) < lp_end) {
                        p += 2;
                        continue;
                    }
                    if (*p == '"') obj_in_str = !obj_in_str;
                    else if (!obj_in_str) {
                        if (*p == '{') obj_depth++;
                        else if (*p == '}') {
                            obj_depth--;
                            if (obj_depth == 0) {
                                obj_end = p;
                                p++;
                                break;
                            }
                        }
                    }
                    p++;
                }
                if (!obj_end) break;

                const char *tn = strstr(obj_start, "\"titleName\"");
                if (tn && tn < obj_end) {
                    const char *tn_colon = strchr(tn + 11, ':');
                    if (tn_colon && tn_colon < obj_end) {
                        const char *q = tn_colon + 1;
                        while (q < obj_end && isspace((unsigned char)*q)) q++;
                        if (q < obj_end && *q == '"') {
                            q++;
                            char tval[PKG_TITLE_NAME_LEN] = {0};
                            size_t tidx = 0;
                            while (q < obj_end && *q != '"') {
                                if (*q == '\\' && (q + 1) < obj_end) q++;
                                if (tidx + 1 < sizeof(tval)) tval[tidx++] = *q;
                                q++;
                            }
                            tval[tidx] = '\0';
                            if (tval[0] && key[0]) {
                                strncpy(entries[count].lang, key, sizeof(entries[count].lang) - 1);
                                entries[count].lang[sizeof(entries[count].lang) - 1] = '\0';
                                strncpy(entries[count].title, tval, sizeof(entries[count].title) - 1);
                                entries[count].title[sizeof(entries[count].title) - 1] = '\0';
                                count++;
                            }
                        }
                    }
                }
            }
        }

        /* Build out_localized_titles JSON string */
        if (out_localized_titles && loc_max > 2 && count > 0) {
            size_t pos = 0;
            out_localized_titles[pos++] = '{';
            for (size_t i = 0; i < count; i++) {
                char esc_t[PKG_TITLE_NAME_LEN * 2];
                size_t eidx = 0;
                for (size_t c = 0; entries[i].title[c] && eidx + 2 < sizeof(esc_t); c++) {
                    if (entries[i].title[c] == '"' || entries[i].title[c] == '\\') {
                        esc_t[eidx++] = '\\';
                    }
                    esc_t[eidx++] = entries[i].title[c];
                }
                esc_t[eidx] = '\0';

                int w = snprintf(out_localized_titles + pos, loc_max - pos,
                                 "%s\"%s\":\"%s\"",
                                 (i > 0 ? "," : ""),
                                 entries[i].lang,
                                 esc_t);
                if (w > 0 && (size_t)w < loc_max - pos) {
                    pos += (size_t)w;
                } else {
                    break;
                }
            }
            if (pos < loc_max - 1) {
                out_localized_titles[pos++] = '}';
                out_localized_titles[pos] = '\0';
            } else {
                out_localized_titles[loc_max - 1] = '\0';
            }
        }

        /* Determine best default title */
        if (out_title_name && tname_max > 0 && out_title_name[0] == '\0') {
            /* 1. Global title if present */
            if (global_title[0]) {
                strncpy(out_title_name, global_title, tname_max - 1);
                out_title_name[tname_max - 1] = '\0';
            }
            /* 2. Title for defaultLanguage */
            else if (def_lang[0]) {
                for (size_t i = 0; i < count; i++) {
                    if (strcmp(entries[i].lang, def_lang) == 0) {
                        strncpy(out_title_name, entries[i].title, tname_max - 1);
                        out_title_name[tname_max - 1] = '\0';
                        break;
                    }
                }
            }
            /* 3. Title for English */
            if (out_title_name[0] == '\0') {
                for (size_t i = 0; i < count; i++) {
                    if (strncmp(entries[i].lang, "en", 2) == 0) {
                        strncpy(out_title_name, entries[i].title, tname_max - 1);
                        out_title_name[tname_max - 1] = '\0';
                        break;
                    }
                }
            }
            /* 4. First localized title */
            if (out_title_name[0] == '\0' && count > 0) {
                strncpy(out_title_name, entries[0].title, tname_max - 1);
                out_title_name[tname_max - 1] = '\0';
            }
        }
    } else {
        /* No localizedParameters: use global title if found */
        if (out_title_name && tname_max > 0 && out_title_name[0] == '\0' && global_title[0]) {
            strncpy(out_title_name, global_title, tname_max - 1);
            out_title_name[tname_max - 1] = '\0';
        }
    }
}

static const char *s_sfo_lang_map[30] = {
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL", "pt-PT",
    "ru-RU", "ko-KR", "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO",
    "pl-PL", "pt-BR", "en-GB", "tr-TR", "es-419", "ar-AE", "fr-CA", "cs-CZ",
    "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID"
};

/* Helper to parse PS4 param.sfo */
void pkg_parser_parse_param_sfo(const uint8_t *sfo, size_t sfo_len, char *out_title, size_t title_max,
                                char *out_title_id, size_t title_id_max,
                                char *out_version, size_t version_max,
                                char *out_category, size_t category_max,
                                char *out_localized_titles, size_t loc_max,
                                char *out_default_lang, size_t def_lang_max) {
    if (sfo_len < 20 || memcmp(sfo, "\x00PSF", 4) != 0) {
        return;
    }

    uint32_t key_table_start = read_le32(sfo + 0x08);
    uint32_t data_table_start = read_le32(sfo + 0x0C);
    uint32_t entry_count = read_le32(sfo + 0x10);

    if (key_table_start >= sfo_len || data_table_start >= sfo_len || entry_count > 1024) {
        return;
    }

    char sfo_app_ver[32] = {0};
    char sfo_version[32] = {0};

    struct {
        char lang[32];
        char title[PKG_TITLE_NAME_LEN];
    } sfo_loc[32];
    size_t sfo_loc_count = 0;

    const uint8_t *entries = sfo + 20;
    for (uint32_t i = 0; i < entry_count; i++) {
        if ((size_t)(20 + (i + 1) * 16) > sfo_len) {
            break;
        }
        const uint8_t *e = entries + i * 16;
        uint16_t key_off = read_le16(e);
        uint32_t data_len = read_le32(e + 4);
        uint32_t data_off = read_le32(e + 12);

        if (key_table_start + key_off >= sfo_len) continue;
        const char *key = (const char *)(sfo + key_table_start + key_off);
        size_t max_key_len = sfo_len - (key_table_start + key_off);
        if (!memchr(key, '\0', max_key_len)) continue;

        if ((uint64_t)data_off > sfo_len || (uint64_t)data_len > sfo_len ||
            (uint64_t)data_table_start + (uint64_t)data_off + (uint64_t)data_len > (uint64_t)sfo_len) continue;
        const char *data = (const char *)(sfo + data_table_start + data_off);

        if (strcmp(key, "TITLE") == 0 && out_title[0] == '\0') {
            size_t copy_len = data_len < title_max ? data_len : title_max - 1;
            /* Strip trailing null if included in data_len */
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(out_title, data, copy_len);
            out_title[copy_len] = '\0';
        } else if (strcmp(key, "TITLE_ID") == 0 && out_title_id[0] == '\0') {
            size_t copy_len = data_len < title_id_max ? data_len : title_id_max - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(out_title_id, data, copy_len);
            out_title_id[copy_len] = '\0';
        } else if (strncmp(key, "TITLE_", 6) == 0 && sfo_loc_count < 32) {
            /* Verify suffix is purely numeric (TITLE_00..TITLE_29), not TITLE_ID etc. */
            const char *suffix = key + 6;
            int is_numeric = (suffix[0] != '\0');
            for (const char *s = suffix; *s; s++) {
                if (*s < '0' || *s > '9') { is_numeric = 0; break; }
            }
            int l_idx = is_numeric ? atoi(suffix) : -1;
            if (l_idx >= 0 && l_idx < 30) {
                size_t copy_len = data_len < PKG_TITLE_NAME_LEN ? data_len : PKG_TITLE_NAME_LEN - 1;
                while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
                strncpy(sfo_loc[sfo_loc_count].lang, s_sfo_lang_map[l_idx], sizeof(sfo_loc[sfo_loc_count].lang) - 1);
                strncpy(sfo_loc[sfo_loc_count].title, data, copy_len);
                sfo_loc[sfo_loc_count].title[copy_len] = '\0';
                sfo_loc_count++;
            }
        } else if (strcmp(key, "CATEGORY") == 0 && out_category && out_category[0] == '\0') {
            size_t copy_len = data_len < category_max ? data_len : category_max - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(out_category, data, copy_len);
            out_category[copy_len] = '\0';
        } else if (strcmp(key, "APP_VER") == 0 && sfo_app_ver[0] == '\0') {
            size_t copy_len = data_len < sizeof(sfo_app_ver) ? data_len : sizeof(sfo_app_ver) - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(sfo_app_ver, data, copy_len);
            sfo_app_ver[copy_len] = '\0';
        } else if (strcmp(key, "VERSION") == 0 && sfo_version[0] == '\0') {
            size_t copy_len = data_len < sizeof(sfo_version) ? data_len : sizeof(sfo_version) - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(sfo_version, data, copy_len);
            sfo_version[copy_len] = '\0';
        }
    }

    /* Fallback title if TITLE wasn't present but TITLE_XX was */
    if (out_title[0] == '\0' && sfo_loc_count > 0) {
        /* Prefer en-US (index 01) if available */
        int found = 0;
        for (size_t i = 0; i < sfo_loc_count; i++) {
            if (strcmp(sfo_loc[i].lang, "en-US") == 0) {
                strncpy(out_title, sfo_loc[i].title, title_max - 1);
                out_title[title_max - 1] = '\0';
                found = 1;
                break;
            }
        }
        if (!found) {
            strncpy(out_title, sfo_loc[0].title, title_max - 1);
            out_title[title_max - 1] = '\0';
        }
    }

    if (sfo_loc_count > 0) {
        if (out_default_lang && def_lang_max > 0 && out_default_lang[0] == '\0') {
            strncpy(out_default_lang, "en-US", def_lang_max - 1);
            out_default_lang[def_lang_max - 1] = '\0';
        }
        if (out_localized_titles && loc_max > 2) {
            size_t pos = 0;
            out_localized_titles[pos++] = '{';
            for (size_t i = 0; i < sfo_loc_count; i++) {
                char esc_t[PKG_TITLE_NAME_LEN * 2];
                size_t eidx = 0;
                for (size_t c = 0; sfo_loc[i].title[c] && eidx + 2 < sizeof(esc_t); c++) {
                    if (sfo_loc[i].title[c] == '"' || sfo_loc[i].title[c] == '\\') {
                        esc_t[eidx++] = '\\';
                    }
                    esc_t[eidx++] = sfo_loc[i].title[c];
                }
                esc_t[eidx] = '\0';
                int w = snprintf(out_localized_titles + pos, loc_max - pos,
                                 "%s\"%s\":\"%s\"",
                                 (i > 0 ? "," : ""),
                                 sfo_loc[i].lang,
                                 esc_t);
                if (w > 0 && (size_t)w < loc_max - pos) {
                    pos += (size_t)w;
                } else {
                    break;
                }
            }
            if (pos < loc_max - 1) {
                out_localized_titles[pos++] = '}';
                out_localized_titles[pos] = '\0';
            } else {
                out_localized_titles[loc_max - 1] = '\0';
            }
        }
    }

    const char *best_v = (sfo_app_ver[0] != '\0') ? sfo_app_ver : sfo_version;
    if (best_v && best_v[0] != '\0' && out_version && out_version[0] == '\0') {
        if (best_v[0] != 'v' && best_v[0] != 'V') {
            snprintf(out_version, version_max, "v%s", best_v);
        } else {
            strncpy(out_version, best_v, version_max - 1);
            out_version[version_max - 1] = '\0';
        }
    }
}

int pkg_parser_parse(const char *file_path, pkg_detail_t *out) {
    if (!file_path || !out) {
        return -1;
    }

    if (strncmp(file_path, "smb://", 6) == 0) {
        return smb_client_parse_pkg(file_path, out);
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->path, file_path, sizeof(out->path) - 1);

    /* Extract filename from path */
    const char *slash = strrchr(file_path, '/');
    if (slash) {
        strncpy(out->filename, slash + 1, sizeof(out->filename) - 1);
    } else {
        strncpy(out->filename, file_path, sizeof(out->filename) - 1);
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) == 0) {
        out->file_size = (uint64_t)st.st_size;
        out->total_pkg_size = (uint64_t)st.st_size;
        out->mtime = (uint64_t)st.st_mtime;
    }

    uint8_t hdr[0x200];
    ssize_t hdr_read = pread(fd, hdr, sizeof(hdr), 0);
    if (hdr_read < 0x80) {
        close(fd);
        return -1;
    }

    /* Check for multi-part archive format (PS5MPKG1) */
    if (hdr_read >= MULTIPART_MAGIC_LEN && memcmp(hdr, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN) == 0) {
        multipart_header_t mhdr;
        if (pread(fd, &mhdr, sizeof(mhdr), 0) == sizeof(mhdr) && multipart_is_valid_header(&mhdr)) {
            close(fd);
            /* Disk-controlled strings may lack NUL termination: force it
             * before any strcmp/strncpy to avoid OOB stack reads. */
            mhdr.pkg_type[sizeof(mhdr.pkg_type) - 1] = '\0';
            mhdr.title_id[sizeof(mhdr.title_id) - 1] = '\0';
            mhdr.title_name[sizeof(mhdr.title_name) - 1] = '\0';
            mhdr.content_id[sizeof(mhdr.content_id) - 1] = '\0';
            mhdr.app_version[sizeof(mhdr.app_version) - 1] = '\0';
            mhdr.pkg_filename[sizeof(mhdr.pkg_filename) - 1] = '\0';
            out->is_multipart = 1;
            out->part_index = mhdr.part_index;
            out->total_parts = mhdr.total_parts;
            if (mhdr.pkg_type[0] != '\0') {
                if (strcmp(mhdr.pkg_type, "update") == 0) {
                    out->pkg_type = PKG_TYPE_UPDATE;
                    strncpy(out->pkg_type_str, "update", sizeof(out->pkg_type_str) - 1);
                } else if (strcmp(mhdr.pkg_type, "dlc") == 0) {
                    out->pkg_type = PKG_TYPE_DLC;
                    strncpy(out->pkg_type_str, "dlc", sizeof(out->pkg_type_str) - 1);
                } else {
                    out->pkg_type = PKG_TYPE_BASE;
                    strncpy(out->pkg_type_str, "base", sizeof(out->pkg_type_str) - 1);
                }
            } else {
                out->pkg_type = PKG_TYPE_BASE;
                strncpy(out->pkg_type_str, "base", sizeof(out->pkg_type_str) - 1);
            }
            if (mhdr.title_id[0] != '\0') {
                strncpy(out->title_id, mhdr.title_id, sizeof(out->title_id) - 1);
                out->title_id[sizeof(out->title_id) - 1] = '\0';
            } else {
                strncpy(out->title_id, "UNKNOWN", sizeof(out->title_id) - 1);
                out->title_id[sizeof(out->title_id) - 1] = '\0';
            }
            if (mhdr.title_name[0] != '\0') {
                strncpy(out->title_name, mhdr.title_name, sizeof(out->title_name) - 1);
                out->title_name[sizeof(out->title_name) - 1] = '\0';
            } else if (out->title_id[0] != '\0') {
                strncpy(out->title_name, out->title_id, sizeof(out->title_name) - 1);
                out->title_name[sizeof(out->title_name) - 1] = '\0';
            } else {
                strncpy(out->title_name, "Unknown Package", sizeof(out->title_name) - 1);
                out->title_name[sizeof(out->title_name) - 1] = '\0';
            }
            if (mhdr.app_version[0] != '\0') {
                strncpy(out->app_version, mhdr.app_version, sizeof(out->app_version) - 1);
                out->app_version[sizeof(out->app_version) - 1] = '\0';
            }
            if (mhdr.content_id[0] != '\0') {
                strncpy(out->content_id, mhdr.content_id, sizeof(out->content_id) - 1);
                out->content_id[sizeof(out->content_id) - 1] = '\0';
            }
            out->file_size = (uint64_t)st.st_size;
            out->total_pkg_size = mhdr.total_pkg_size > 0 ? mhdr.total_pkg_size : out->file_size;
            /* Cap icon size: crafted headers with multi-GB sizes would OOM
             * the scanner (payload heap is only a few hundred MB). */
            if (mhdr.icon_offset > 0 && mhdr.icon_size > 0 &&
                mhdr.icon_size < 10 * 1024 * 1024) {
                out->icon_offset = mhdr.icon_offset;
                out->icon_size = mhdr.icon_size;
                out->has_icon = 1;
            } else {
                out->icon_offset = 0;
                out->icon_size = 0;
                out->has_icon = 0;
            }
            out->is_valid = 1;
            return 0;
        }
    }

    uint64_t cnt_offset = 0;
    int cnt_found = 0;

    if (memcmp(hdr, "\x7f" "CNT", 4) == 0) {
        /* Standard PS4 / PS5 CNT at root */
        cnt_offset = 0;
        cnt_found = 1;
    } else if (memcmp(hdr, "\x7f" "FIH", 4) == 0) {
        /* PS5 Package format: check standard entry 2 at offset 0x58 */
        uint64_t cand = read_le64(hdr + 0x58);
        if (cand > 0 && cand < out->file_size) {
            uint8_t test_magic[4];
            if (pread(fd, test_magic, 4, cand) == 4 && memcmp(test_magic, "\x7f" "CNT", 4) == 0) {
                cnt_offset = cand;
                cnt_found = 1;
            }
        }

        /* If not at 0x58, scan table entries in FIH header (64-byte or 32-byte records) */
        if (!cnt_found) {
            for (size_t off = 0x10; off + 8 <= (size_t)hdr_read; off += 0x08) {
                cand = read_le64(hdr + off);
                if (cand >= 0x10000 && cand < out->file_size && (cand % 0x1000) == 0) {
                    uint8_t test_magic[4];
                    if (pread(fd, test_magic, 4, cand) == 4 && memcmp(test_magic, "\x7f" "CNT", 4) == 0) {
                        cnt_offset = cand;
                        cnt_found = 1;
                        break;
                    }
                }
            }
        }
    }

    if (!cnt_found) {
        close(fd);
        return -1;
    }

    /* Read CNT Header */
    uint8_t cnt_hdr[0x80];
    if (pread(fd, cnt_hdr, sizeof(cnt_hdr), cnt_offset) != sizeof(cnt_hdr)) {
        close(fd);
        return -1;
    }

    if (memcmp(cnt_hdr, "\x7f" "CNT", 4) != 0) {
        close(fd);
        return -1;
    }

    uint32_t cnt_type_magic = read_be32(cnt_hdr + 0x04);

    /* Extract Content ID at 0x40 (up to 48 bytes) */
    memcpy(out->content_id, cnt_hdr + 0x40, 48);
    out->content_id[48] = '\0';
    for (int i = 0; i < 48; i++) {
        if ((unsigned char)out->content_id[i] < 32 || (unsigned char)out->content_id[i] > 126) {
            out->content_id[i] = '\0';
            break;
        }
    }

    uint32_t entry_count = read_be32(cnt_hdr + 0x10);
    uint32_t table_offset = read_be32(cnt_hdr + 0x18);

    if (entry_count == 0 || entry_count > 2048 || table_offset > 0x200000) {
        close(fd);
        return -1;
    }

    size_t table_size = (size_t)entry_count * 32;
    /* Pre-check table fits inside the file before malloc+pread, so crafted
     * >4GB offsets can't cause wrong parses or wasted I/O on optical. */
    if (out->file_size > 0 &&
        (uint64_t)cnt_offset + (uint64_t)table_offset + (uint64_t)table_size > out->file_size) {
        close(fd);
        return -1;
    }
    uint8_t *entry_table = (uint8_t *)malloc(table_size);
    if (!entry_table) {
        close(fd);
        return -1;
    }

    if (pread(fd, entry_table, table_size, cnt_offset + table_offset) != (ssize_t)table_size) {
        free(entry_table);
        close(fd);
        return -1;
    }

    /* Locate string table (type 0x0200) */
    uint32_t str_table_off = 0;
    uint32_t str_table_sz = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        uint32_t type = read_be32(e);
        if (type == 0x0200) {
            str_table_off = read_be32(e + 16);
            str_table_sz = read_be32(e + 20);
            break;
        }
    }

    char *str_table = NULL;
    if (str_table_sz > 0 && str_table_sz < 65536) {
        str_table = (char *)malloc(str_table_sz + 1);
        if (str_table) {
            if (pread(fd, str_table, str_table_sz, cnt_offset + str_table_off) == (ssize_t)str_table_sz) {
                str_table[str_table_sz] = '\0';
            } else {
                free(str_table);
                str_table = NULL;
            }
        }
    }

    int has_playgo_chunk_patch = 0;
    int has_delta_patch = 0;
    int has_base_app_metadata = 0;

    /* Iterate entries to find param.json, param.sfo, and icon0.png */
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        uint32_t type = read_be32(e);
        uint32_t fn_off = read_be32(e + 4);
        uint32_t data_off = read_be32(e + 16);
        uint32_t data_sz = read_be32(e + 20);

        const char *name = "";
        if (str_table && fn_off < str_table_sz) {
            name = str_table + fn_off;
        }

        if (type == 0x1008 || strcmp(name, "app/playgo-chunk.dat") == 0) {
            has_playgo_chunk_patch = 1;
        }
        if (type == 0x0407 || type == 0x0408 ||
            strcmp(name, "target-deltainfo.dat") == 0 || strcmp(name, "origin-deltainfo.dat") == 0) {
            has_delta_patch = 1;
        }

        /* 1. param.json (PS5) */
        if ((type == 0x2000 || strcmp(name, "param.json") == 0) && data_sz > 0 && data_sz < 262144) {
            char *json_buf = (char *)malloc(data_sz + 1);
            if (json_buf) {
                if (pread(fd, json_buf, data_sz, cnt_offset + data_off) == (ssize_t)data_sz) {
                    json_buf[data_sz] = '\0';
                    if (json_has_key(json_buf, data_sz, "applicationDrmType") ||
                        json_has_key(json_buf, data_sz, "applicationCategoryType") ||
                        json_has_key(json_buf, data_sz, "contentBadgeType")) {
                        has_base_app_metadata = 1;
                    }
                    pkg_parser_parse_param_json(json_buf, data_sz,
                                                out->title_id, sizeof(out->title_id),
                                                out->title_name, sizeof(out->title_name),
                                                out->category, sizeof(out->category),
                                                out->app_version, sizeof(out->app_version),
                                                out->localized_titles, sizeof(out->localized_titles),
                                                out->default_language, sizeof(out->default_language));
                }
                free(json_buf);
            }
        }

        /* 2. param.sfo (PS4) */
        if ((type == 0x1000 || strcmp(name, "param.sfo") == 0) && data_sz > 0 && data_sz < 262144) {
            uint8_t *sfo_buf = (uint8_t *)malloc(data_sz);
            if (sfo_buf) {
                if (pread(fd, sfo_buf, data_sz, cnt_offset + data_off) == (ssize_t)data_sz) {
                    char stitle[PKG_TITLE_NAME_LEN] = {0};
                    char stid[PKG_TITLE_ID_LEN] = {0};
                    char sver[32] = {0};
                    pkg_parser_parse_param_sfo(sfo_buf, data_sz, stitle, sizeof(stitle), stid, sizeof(stid), sver, sizeof(sver),
                                                out->category, sizeof(out->category),
                                                out->localized_titles, sizeof(out->localized_titles),
                                                out->default_language, sizeof(out->default_language));
                    if (out->title_id[0] == '\0' && stid[0] != '\0') {
                        strncpy(out->title_id, stid, sizeof(out->title_id) - 1);
                    }
                    if (out->title_name[0] == '\0' && stitle[0] != '\0') {
                        strncpy(out->title_name, stitle, sizeof(out->title_name) - 1);
                    }
                    if (out->app_version[0] == '\0' && sver[0] != '\0') {
                        strncpy(out->app_version, sver, sizeof(out->app_version) - 1);
                    }
                }
                free(sfo_buf);
            }
        }

        /* 3. icon0.png: cap size so a crafted table can't trigger a ~4GB
         * malloc downstream, and validate the range fits inside the file. */
        if (type == 0x1200 || strcmp(name, "icon0.png") == 0) {
            if (data_sz > 0 && data_sz < 10 * 1024 * 1024 &&
                (out->file_size == 0 ||
                 (uint64_t)cnt_offset + (uint64_t)data_off + (uint64_t)data_sz <= out->file_size)) {
                out->icon_offset = cnt_offset + data_off;
                out->icon_size = data_sz;
                out->has_icon = 1;
            }
        }
    }

    int is_delta_type = ((cnt_type_magic & 0xFF) == 0x1E || (cnt_type_magic & 0xFF000000) == 0x41000000);

    if (has_playgo_chunk_patch || has_delta_patch || is_delta_type ||
        (out->category[0] != '\0' && strncmp(out->category, "gp", 2) == 0)) {
        out->pkg_type = PKG_TYPE_UPDATE;
    } else if ((cnt_type_magic & 0xFF) == 1 && !has_base_app_metadata) {
        /* Some PS5 DLC metadata variants omit param.json.category and the
         * normal application fields. The CNT field carries flags in its
         * upper bytes, so only inspect its low-byte type after excluding a
         * normal base-application param.json. */
        out->pkg_type = PKG_TYPE_DLC;
    } else if (out->category[0] != '\0') {
        if (strncmp(out->category, "ac", 2) == 0 || strncmp(out->category, "al", 2) == 0 ||
            strcmp(out->category, "addcont") == 0) {
            out->pkg_type = PKG_TYPE_DLC;
        } else if (strncmp(out->category, "gd", 2) == 0 || strncmp(out->category, "bd", 2) == 0 ||
                   strncmp(out->category, "gc", 2) == 0 || strncmp(out->category, "wt", 2) == 0) {
            out->pkg_type = PKG_TYPE_BASE;
        }
    }

    if (out->pkg_type == PKG_TYPE_UNKNOWN) {
        out->pkg_type = PKG_TYPE_BASE;
    }

    switch (out->pkg_type) {
        case PKG_TYPE_BASE:
            strncpy(out->pkg_type_str, "base", sizeof(out->pkg_type_str) - 1);
            break;
        case PKG_TYPE_UPDATE:
            strncpy(out->pkg_type_str, "update", sizeof(out->pkg_type_str) - 1);
            break;
        case PKG_TYPE_DLC:
            strncpy(out->pkg_type_str, "dlc", sizeof(out->pkg_type_str) - 1);
            break;
        default:
            strncpy(out->pkg_type_str, "unknown", sizeof(out->pkg_type_str) - 1);
            break;
    }

    if (str_table) {
        free(str_table);
    }
    free(entry_table);
    close(fd);

    /* Fallbacks if title_id or title_name could not be found */
    if (out->title_id[0] == '\0' && out->content_id[0] != '\0') {
        /* Often content_id is XX0000-TITLEID_00-... */
        const char *dash = strchr(out->content_id, '-');
        if (dash) {
            const char *us = strchr(dash + 1, '_');
            if (us && (size_t)(us - (dash + 1)) < sizeof(out->title_id)) {
                size_t len = us - (dash + 1);
                strncpy(out->title_id, dash + 1, len);
                out->title_id[len] = '\0';
            }
        }
    }

    if (out->title_name[0] == '\0') {
        if (out->title_id[0] != '\0') {
            snprintf(out->title_name, sizeof(out->title_name), "%s", out->title_id);
        } else {
            snprintf(out->title_name, sizeof(out->title_name), "Unknown Package");
        }
    }

    out->is_valid = 1;
    return 0;
}

/* Bounded slice reader for the mem parser: NULL when [off, off+sz) is not
 * fully inside [data, data+data_len). Fail-closed, overflow-safe. */
static const uint8_t *mem_slice(const uint8_t *data, size_t data_len,
                                uint64_t off, uint64_t sz) {
    if (sz == 0 || off > data_len) return NULL;
    if (sz > data_len || off > data_len - sz) return NULL;
    return data + off;
}

int pkg_parser_parse_mem(const uint8_t *data, size_t data_len,
                         uint64_t total_size, const char *filename,
                         pkg_detail_t *out, int *out_stage) {
#define PM_STAGE(n) do { if (out_stage) *out_stage = (n); } while (0)
    if (!data || data_len == 0 || !out) {
        PM_STAGE(1);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (filename && filename[0] != '\0') {
        const char *slash = strchr(filename, '/');
        const char *base = slash ? slash + 1 : filename;
        /* Live URIs look like "live:<id>": keep a friendly display name. */
        if (strncmp(base, "live:", 5) == 0) {
            snprintf(out->filename, sizeof(out->filename), "%.200s.pkg", base);
            snprintf(out->path, sizeof(out->path), "%s", base);
        } else {
            strncpy(out->filename, base, sizeof(out->filename) - 1);
            strncpy(out->path, filename, sizeof(out->path) - 1);
        }
    }
    out->file_size = total_size;
    out->total_pkg_size = total_size;
    out->pkg_type = PKG_TYPE_UNKNOWN;

    const uint8_t *hdr = mem_slice(data, data_len, 0, 0x80);
    if (!hdr) {
        PM_STAGE(1);
        return -1;
    }

    /* Multi-part container: same field mapping as the file parser. */
    if (data_len >= MULTIPART_MAGIC_LEN &&
        memcmp(data, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN) == 0) {
        const uint8_t *mp = mem_slice(data, data_len, 0, sizeof(multipart_header_t));
        if (!mp) {
            return -1;
        }
        multipart_header_t mhdr;
        memcpy(&mhdr, mp, sizeof(mhdr));
        if (!multipart_is_valid_header(&mhdr)) {
            return -1;
        }
        mhdr.pkg_type[sizeof(mhdr.pkg_type) - 1] = '\0';
        mhdr.title_id[sizeof(mhdr.title_id) - 1] = '\0';
        mhdr.title_name[sizeof(mhdr.title_name) - 1] = '\0';
        mhdr.content_id[sizeof(mhdr.content_id) - 1] = '\0';
        mhdr.app_version[sizeof(mhdr.app_version) - 1] = '\0';
        out->is_multipart = 1;
        out->part_index = mhdr.part_index;
        out->total_parts = mhdr.total_parts;
        if (strcmp(mhdr.pkg_type, "update") == 0) {
            out->pkg_type = PKG_TYPE_UPDATE;
            strncpy(out->pkg_type_str, "update", sizeof(out->pkg_type_str) - 1);
        } else if (strcmp(mhdr.pkg_type, "dlc") == 0) {
            out->pkg_type = PKG_TYPE_DLC;
            strncpy(out->pkg_type_str, "dlc", sizeof(out->pkg_type_str) - 1);
        } else {
            out->pkg_type = PKG_TYPE_BASE;
            strncpy(out->pkg_type_str, "base", sizeof(out->pkg_type_str) - 1);
        }
        if (mhdr.title_id[0] != '\0') {
            strncpy(out->title_id, mhdr.title_id, sizeof(out->title_id) - 1);
        } else {
            strncpy(out->title_id, "UNKNOWN", sizeof(out->title_id) - 1);
        }
        if (mhdr.title_name[0] != '\0') {
            strncpy(out->title_name, mhdr.title_name, sizeof(out->title_name) - 1);
        } else {
            strncpy(out->title_name, out->title_id, sizeof(out->title_name) - 1);
        }
        if (mhdr.app_version[0] != '\0') {
            strncpy(out->app_version, mhdr.app_version, sizeof(out->app_version) - 1);
        }
        if (mhdr.content_id[0] != '\0') {
            strncpy(out->content_id, mhdr.content_id, sizeof(out->content_id) - 1);
        }
        if (mhdr.total_pkg_size > 0) {
            out->total_pkg_size = mhdr.total_pkg_size;
        }
        out->has_icon = 0;
        out->is_valid = 1;
        PM_STAGE(0);
        return 0;
    }

    uint64_t cnt_offset = 0;
    int cnt_found = 0;
    if (memcmp(hdr, "\x7f" "CNT", 4) == 0) {
        cnt_offset = 0;
        cnt_found = 1;
    } else if (memcmp(hdr, "\x7f" "FIH", 4) == 0) {
        const uint8_t *e58 = mem_slice(data, data_len, 0x58, 8);
        if (e58) {
            uint64_t cand = read_le64(e58);
            const uint8_t *t = mem_slice(data, data_len, cand, 4);
            /* Parity with the file parser: candidate must be a plausible
             * in-file offset (positive, below total size). */
            if (cand > 0 && (total_size == 0 || cand < total_size) &&
                t && memcmp(t, "\x7f" "CNT", 4) == 0) {
                cnt_offset = cand;
                cnt_found = 1;
            }
        }
        if (!cnt_found) {
            /* Parity: the file parser scans only its 0x200-byte header
             * window, so a coincidental match deeper in the buffer must
             * not win here either. */
            uint64_t scan_end = data_len < 0x200 ? data_len : 0x200;
            for (uint64_t off = 0x10; off + 8 <= scan_end; off += 0x08) {
                const uint8_t *e = mem_slice(data, data_len, off, 8);
                if (!e) break;
                uint64_t cand = read_le64(e);
                if (cand >= 0x10000 && (total_size == 0 || cand < total_size) &&
                    (cand % 0x1000) == 0) {
                    const uint8_t *t = mem_slice(data, data_len, cand, 4);
                    if (t && memcmp(t, "\x7f" "CNT", 4) == 0) {
                        cnt_offset = cand;
                        cnt_found = 1;
                        break;
                    }
                }
            }
        }
    }
    if (!cnt_found) {
        PM_STAGE(2);
        return -1;
    }

    const uint8_t *cnt_hdr = mem_slice(data, data_len, cnt_offset, 0x80);
    if (!cnt_hdr || memcmp(cnt_hdr, "\x7f" "CNT", 4) != 0) {
        PM_STAGE(3);
        return -1;
    }
    uint32_t cnt_type_magic = read_be32(cnt_hdr + 0x04);

    const uint8_t *cid = mem_slice(data, data_len, cnt_offset + 0x40, 48);
    if (!cid) {
        PM_STAGE(4);
        return -1;
    }
    memcpy(out->content_id, cid, 48);
    out->content_id[48] = '\0';
    for (int i = 0; i < 48; i++) {
        if ((unsigned char)out->content_id[i] < 32 || (unsigned char)out->content_id[i] > 126) {
            out->content_id[i] = '\0';
            break;
        }
    }

    uint32_t entry_count = read_be32(cnt_hdr + 0x10);
    uint32_t table_offset = read_be32(cnt_hdr + 0x18);
    if (entry_count == 0 || entry_count > 2048 || table_offset > 0x200000) {
        PM_STAGE(5);
        return -1;
    }
    uint64_t table_size = (uint64_t)entry_count * 32;
    const uint8_t *entry_table = mem_slice(data, data_len,
                                           cnt_offset + table_offset, table_size);
    if (!entry_table) {
        PM_STAGE(6); /* table beyond the cached prefix */
        return -1;
    }

    uint32_t str_table_off = 0;
    uint32_t str_table_sz = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        if (read_be32(e) == 0x0200) {
            str_table_off = read_be32(e + 16);
            str_table_sz = read_be32(e + 20);
            break;
        }
    }
    const char *str_table = NULL;
    if (str_table_sz > 0 && str_table_sz < 65536) {
        str_table = (const char *)mem_slice(data, data_len,
                                            cnt_offset + str_table_off,
                                            str_table_sz);
    }

    int has_playgo_chunk_patch = 0;
    int has_delta_patch = 0;
    int has_base_app_metadata = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = entry_table + i * 32;
        uint32_t type = read_be32(e);
        uint32_t fn_off = read_be32(e + 4);
        uint32_t data_off = read_be32(e + 16);
        uint32_t data_sz = read_be32(e + 20);

        const char *name = "";
        char name_tmp[128] = {0};
        if (str_table && fn_off < str_table_sz) {
            size_t nl = strnlen(str_table + fn_off, str_table_sz - fn_off);
            if (nl < sizeof(name_tmp)) {
                memcpy(name_tmp, str_table + fn_off, nl + 1);
                name = name_tmp;
            }
        }

        if (type == 0x1008 || strcmp(name, "app/playgo-chunk.dat") == 0) {
            has_playgo_chunk_patch = 1;
        }
        if (type == 0x0407 || type == 0x0408 ||
            strcmp(name, "target-deltainfo.dat") == 0 || strcmp(name, "origin-deltainfo.dat") == 0) {
            has_delta_patch = 1;
        }

        /* 1. param.json (PS5) */
        if ((type == 0x2000 || strcmp(name, "param.json") == 0) && data_sz > 0 && data_sz < 262144) {
            const uint8_t *jb = mem_slice(data, data_len, cnt_offset + data_off, data_sz);
            if (jb) {
                if (json_has_key((const char *)jb, data_sz, "applicationDrmType") ||
                    json_has_key((const char *)jb, data_sz, "applicationCategoryType") ||
                    json_has_key((const char *)jb, data_sz, "contentBadgeType")) {
                    has_base_app_metadata = 1;
                }
                char tid[PKG_TITLE_ID_LEN] = {0};
                char tname[PKG_TITLE_NAME_LEN] = {0};
                if (json_extract_key((const char *)jb, data_sz, "titleId", tid, sizeof(tid)) == 0) {
                    strncpy(out->title_id, tid, sizeof(out->title_id) - 1);
                }
                if (json_extract_key((const char *)jb, data_sz, "titleName", tname, sizeof(tname)) == 0) {
                    strncpy(out->title_name, tname, sizeof(out->title_name) - 1);
                }
                char cat_buf[16] = {0};
                if (json_extract_key((const char *)jb, data_sz, "category", cat_buf, sizeof(cat_buf)) == 0 && out->category[0] == '\0') {
                    strncpy(out->category, cat_buf, sizeof(out->category) - 1);
                }
                char ver[32] = {0};
                if (json_extract_key((const char *)jb, data_sz, "contentVersion", ver, sizeof(ver)) == 0 ||
                    json_extract_key((const char *)jb, data_sz, "appVersion", ver, sizeof(ver)) == 0 ||
                    json_extract_key((const char *)jb, data_sz, "version", ver, sizeof(ver)) == 0) {
                    if (ver[0] != '\0') {
                        int maj = 0, min = 0, patch = 0;
                        if (sscanf(ver, "%d.%d.%d", &maj, &min, &patch) == 3) {
                            if (min == 0 && patch > 0) {
                                snprintf(out->app_version, sizeof(out->app_version), "v%d.%02d", maj, patch);
                            } else if (patch == 0) {
                                snprintf(out->app_version, sizeof(out->app_version), "v%d.%02d", maj, min);
                            } else {
                                snprintf(out->app_version, sizeof(out->app_version), "v%d.%d.%d", maj, min, patch);
                            }
                        } else if (ver[0] != 'v' && ver[0] != 'V') {
                            snprintf(out->app_version, sizeof(out->app_version), "v%.29s", ver);
                        } else {
                            strncpy(out->app_version, ver, sizeof(out->app_version) - 1);
                        }
                    }
                }
            }
        }

        /* 2. param.sfo (PS4) */
        if ((type == 0x1000 || strcmp(name, "param.sfo") == 0) && data_sz > 0 && data_sz < 262144) {
            const uint8_t *sb = mem_slice(data, data_len, cnt_offset + data_off, data_sz);
            if (sb) {
                char stitle[PKG_TITLE_NAME_LEN] = {0};
                char stid[PKG_TITLE_ID_LEN] = {0};
                char sver[32] = {0};
                char sloc[PKG_LOCALIZED_TITLES_LEN] = {0};
                char slang[PKG_DEFAULT_LANG_LEN] = {0};
                pkg_parser_parse_param_sfo(sb, data_sz, stitle, sizeof(stitle), stid, sizeof(stid), sver, sizeof(sver),
                                out->category, sizeof(out->category),
                                sloc, sizeof(sloc), slang, sizeof(slang));
                if (out->title_id[0] == '\0' && stid[0] != '\0') {
                    strncpy(out->title_id, stid, sizeof(out->title_id) - 1);
                }
                if (out->title_name[0] == '\0' && stitle[0] != '\0') {
                    strncpy(out->title_name, stitle, sizeof(out->title_name) - 1);
                }
                if (out->app_version[0] == '\0' && sver[0] != '\0') {
                    strncpy(out->app_version, sver, sizeof(out->app_version) - 1);
                }
                if (out->localized_titles[0] == '\0' && sloc[0] != '\0') {
                    strncpy(out->localized_titles, sloc, sizeof(out->localized_titles) - 1);
                }
                if (out->default_language[0] == '\0' && slang[0] != '\0') {
                    strncpy(out->default_language, slang, sizeof(out->default_language) - 1);
                }
            }
        }
        /* 3. icon0.png: skipped for live sessions (has_icon stays 0). */
    }

    int is_delta_type = ((cnt_type_magic & 0xFF) == 0x1E || (cnt_type_magic & 0xFF000000) == 0x41000000);

    if (has_playgo_chunk_patch || has_delta_patch || is_delta_type ||
        (out->category[0] != '\0' && strncmp(out->category, "gp", 2) == 0)) {
        out->pkg_type = PKG_TYPE_UPDATE;
    } else if ((cnt_type_magic & 0xFF) == 1 && !has_base_app_metadata) {
        out->pkg_type = PKG_TYPE_DLC;
    } else if (out->category[0] != '\0') {
        if (strncmp(out->category, "ac", 2) == 0 || strncmp(out->category, "al", 2) == 0 ||
            strcmp(out->category, "addcont") == 0) {
            out->pkg_type = PKG_TYPE_DLC;
        } else if (strncmp(out->category, "gd", 2) == 0 || strncmp(out->category, "bd", 2) == 0 ||
                   strncmp(out->category, "gc", 2) == 0 || strncmp(out->category, "wt", 2) == 0) {
            out->pkg_type = PKG_TYPE_BASE;
        }
    }

    if (out->pkg_type == PKG_TYPE_UNKNOWN) {
        out->pkg_type = PKG_TYPE_BASE;
    }

    switch (out->pkg_type) {
        case PKG_TYPE_BASE:
            strncpy(out->pkg_type_str, "base", sizeof(out->pkg_type_str) - 1);
            break;
        case PKG_TYPE_UPDATE:
            strncpy(out->pkg_type_str, "update", sizeof(out->pkg_type_str) - 1);
            break;
        case PKG_TYPE_DLC:
            strncpy(out->pkg_type_str, "dlc", sizeof(out->pkg_type_str) - 1);
            break;
        default:
            strncpy(out->pkg_type_str, "unknown", sizeof(out->pkg_type_str) - 1);
            break;
    }

    if (out->title_id[0] == '\0' && out->content_id[0] != '\0') {
        const char *dash = strchr(out->content_id, '-');
        if (dash) {
            const char *us = strchr(dash + 1, '_');
            if (us && (size_t)(us - (dash + 1)) < sizeof(out->title_id)) {
                size_t len = us - (dash + 1);
                strncpy(out->title_id, dash + 1, len);
                out->title_id[len] = '\0';
            }
        }
    }

    if (out->title_name[0] == '\0') {
        if (out->title_id[0] != '\0') {
            snprintf(out->title_name, sizeof(out->title_name), "%s", out->title_id);
        } else {
            snprintf(out->title_name, sizeof(out->title_name), "Unknown Package");
        }
    }

    out->has_icon = 0;
    out->is_valid = 1;
    PM_STAGE(0);
    return 0;
#undef PM_STAGE
}

int pkg_parser_get_icon(const char *file_path, uint64_t offset, uint32_t size,
                        uint8_t **out_data, size_t *out_size) {
    if (!file_path || !out_data || !out_size) {
        return -1;
    }

    if (strncmp(file_path, "smb://", 6) == 0) {
        /* When offset/size are already known (e.g. from the scanner cache),
           read directly without re-parsing the entire PKG over SMB. */
        if (offset > 0 && size > 0 && size < 10 * 1024 * 1024) {
            uint8_t *buf = (uint8_t *)malloc(size);
            if (!buf) return -1;
            ssize_t n = smb_client_pread(file_path, buf, size, offset);
            if (n == (ssize_t)size) {
                *out_data = buf;
                *out_size = size;
                return 0;
            }
            free(buf);
        }
        return smb_client_get_icon(file_path, out_data, out_size);
    }

    if (offset == 0 || size == 0 || size >= 10 * 1024 * 1024) {
        return -1;
    }

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st_icon;
    if (fstat(fd, &st_icon) == 0) {
        if (st_icon.st_size <= 0 ||
            offset >= (uint64_t)st_icon.st_size ||
            (uint64_t)size > (uint64_t)st_icon.st_size - offset) {
            close(fd);
            return -1;
        }
    }

    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        close(fd);
        return -1;
    }

    if (pread(fd, data, size, offset) != (ssize_t)size) {
        free(data);
        close(fd);
        return -1;
    }

    close(fd);
    *out_data = data;
    *out_size = size;
    return 0;
}
