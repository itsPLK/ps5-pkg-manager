#ifndef PKG_CACHE_H
#define PKG_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include "pkg_parser.h"
#include "smb_client.h"

#define PKG_CACHE_DIR_DEFAULT "/data/pkgmgr/cache"

typedef struct {
    int move_installed_to_end;
    int fade_installed_packages;
    int all_sources_mode;
    int pkg_install_debug;  /* 1 = log every stream server connection to /data/pkgmgr/ */
    smb_share_config_t smb_shares[MAX_SMB_SHARES];
    int smb_share_count;
} app_settings_t;

/* Initialize caching system and load settings */
void pkg_cache_init(void);

/* Settings get / set */
void pkg_cache_get_settings(app_settings_t *out_settings);
int pkg_cache_set_settings(const app_settings_t *settings);
void pkg_cache_parse_smb_shares(const char *json_buf, smb_share_config_t *shares, int *count);

/* Get directory path for package cache (uses /data/pkgmgr/cache or env/tmp fallback) */
const char *pkg_cache_get_dir(void);

/* Compute checksum string for a package file (combines size, mtime, and header CRC32) */
int pkg_cache_calc_checksum(const char *pkg_path, char *out_checksum, size_t out_max);

/* Cache lookup: returns 0 on hit, -1 on miss */
int pkg_cache_lookup(const char *checksum, pkg_detail_t *out_detail);

/* Cache save: writes meta.json and extracts/saves icon.png if has_icon.
 * On success with icon bytes available, fills in detail->blurhash (detail is
 * non-const so the hash propagates to the in-memory package in the same scan). */
int pkg_cache_save(const char *checksum, pkg_detail_t *detail);

/* Update metadata (meta.json) only: does not re-extract icon.png or re-read PKG.
 * Used for fast in-place BlurHash self-healing / backfill. */
int pkg_cache_update_meta(const char *checksum, const pkg_detail_t *detail);

/* Direct icon retrieval from cache: returns 0 on hit with malloc'd buffer, -1 on miss */
int pkg_cache_get_icon(const char *pkg_path, uint8_t **out_data, size_t *out_size);

/* Get cache statistics JSON */
char *pkg_cache_get_stats_json(void);

/* Clear central cache. Returns total bytes freed. */
int64_t pkg_cache_clear(void);

#endif /* PKG_CACHE_H */

