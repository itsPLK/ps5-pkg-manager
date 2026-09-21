#ifndef PKG_PARSER_H
#define PKG_PARSER_H

#include <stdint.h>
#include <stddef.h>

#define PKG_TITLE_ID_LEN 32
#define PKG_TITLE_NAME_LEN 256
#define PKG_LOCALIZED_TITLES_LEN 2048
#define PKG_DEFAULT_LANG_LEN 32
#define PKG_CONTENT_ID_LEN 64
#define PKG_PATH_LEN 512

typedef enum {
    PKG_TYPE_UNKNOWN = 0,
    PKG_TYPE_BASE,     /* Base application / package */
    PKG_TYPE_UPDATE,   /* Application update / patch */
    PKG_TYPE_DLC       /* Additional content / DLC */
} pkg_type_t;

typedef struct {
    char path[PKG_PATH_LEN];
    char filename[256];
    char title_id[PKG_TITLE_ID_LEN];
    char title_name[PKG_TITLE_NAME_LEN];
    char localized_titles[PKG_LOCALIZED_TITLES_LEN]; /* JSON object string: {"ar-AE":"...","en-US":"..."} */
    char default_language[PKG_DEFAULT_LANG_LEN];     /* e.g. "en-US" */
    char content_id[PKG_CONTENT_ID_LEN];
    char app_version[32];
    uint64_t file_size;
    uint64_t total_pkg_size;
    uint64_t icon_offset;
    uint32_t icon_size;
    int has_icon;
    int is_valid;
    int is_multipart;
    uint32_t part_index;
    uint32_t total_parts;
    pkg_type_t pkg_type;
    char pkg_type_str[16];   /* "base", "update", "dlc", "unknown" */
    char category[16];       /* e.g. "gd", "gp", "ac", etc. */
    uint64_t mtime;          /* File modification timestamp */
    char blurhash[64];       /* BlurHash placeholder for icon0.png ("" if none) */
} pkg_detail_t;

/**
 * Resolves the best-matching title from a localized_titles JSON object given
 * an Accept-Language header string (or comma-separated list of languages).
 * Falls back to default_lang, English, or the first localized title.
 * Returns 0 on success and populates out, or negative on failure.
 */
int pkg_parser_resolve_localized_title(const char *loc_json, const char *default_lang,
                                       const char *accept_lang, char *out, size_t out_max);

/**
 * Parses PS5 param.json buffer to extract titleId, category, version,
 * localized titles map, default language, and best default title.
 */
void pkg_parser_parse_param_json(const char *json_buf, size_t data_sz,
                                 char *out_title_id, size_t tid_max,
                                 char *out_title_name, size_t tname_max,
                                 char *out_category, size_t cat_max,
                                 char *out_version, size_t ver_max,
                                 char *out_localized_titles, size_t loc_max,
                                 char *out_default_lang, size_t def_lang_max);

/**
 * Parses PS4 param.sfo buffer to extract titleId, category, version,
 * localized titles map, default language, and default title.
 */
void pkg_parser_parse_param_sfo(const uint8_t *sfo, size_t sfo_len, char *out_title, size_t title_max,
                                char *out_title_id, size_t title_id_max,
                                char *out_version, size_t version_max,
                                char *out_category, size_t category_max,
                                char *out_localized_titles, size_t loc_max,
                                char *out_default_lang, size_t def_lang_max);

/**
 * Parses a PKG file (PS5 FIH or PS4 CNT format) and extracts metadata:
 * title_id, title_name, content_id, and icon0.png offset/size.
 * Returns 0 on success, negative value on error.
 */
int pkg_parser_parse(const char *file_path, pkg_detail_t *out);

/**
 * Parses PKG metadata from an in-memory prefix (live RAM session): same
 * fields as pkg_parser_parse but bounded to [data, data+data_len) instead
 * of pread(). Anything addressed outside the prefix fails closed (with a
 * stage code in *out_stage when non-NULL: 1 short header, 2 no CNT,
 * 3 CNT header, 4 content_id, 5 entry header, 6 table past prefix).
 * Icon extraction is skipped (has_icon = 0); total sizes come from
 * total_size. Purely additive: the file-based parser above is untouched.
 */
int pkg_parser_parse_mem(const uint8_t *data, size_t data_len,
                         uint64_t total_size, const char *filename,
                         pkg_detail_t *out, int *out_stage);

/**
 * Extracts raw icon bytes from the PKG file.
 * Allocates *out_data with malloc, which caller must free().
 * Returns 0 on success, negative on error.
 */
int pkg_parser_get_icon(const char *file_path, uint64_t offset, uint32_t size,
                        uint8_t **out_data, size_t *out_size);

#endif /* PKG_PARSER_H */
