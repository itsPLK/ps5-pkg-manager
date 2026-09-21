#ifndef PKG_SCANNER_H
#define PKG_SCANNER_H

#include <stddef.h>
#include "pkg_parser.h"

#define PKG_DEFAULT_DIR "/data/pkg"
#define PKG_DISC_DIR    "/mnt/disc/pkg"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the package scanner.
 */
void pkg_scanner_init(void);

/**
 * Scans the package directories (/data/pkg and /mnt/disc/pkg) for .pkg files.
 * Returns the number of valid packages found.
 */
int pkg_scanner_scan(void);

/**
 * Returns the number of packages currently in the cache.
 */
size_t pkg_scanner_get_count(void);

/**
 * Retrieves a copy of package details at index (0 <= index < count).
 * Returns 0 on success, -1 if index is out of range.
 */
int pkg_scanner_get_at(size_t index, pkg_detail_t *out);

/**
 * Finds a package by path.
 * Returns 0 on success, -1 if not found.
 */
int pkg_scanner_find_by_path(const char *path, pkg_detail_t *out);

/* Evaluate a package against the console's current installed state. */
typedef struct {
    int can_install;
    const char *disabled_reason;
    int is_installed;
    char installed_version[32];
} pkg_install_eligibility_t;

void pkg_scanner_check_install_eligibility(const pkg_detail_t *pkg,
                                           pkg_install_eligibility_t *out);

/**
 * Updates the cached BlurHash placeholder for a package in memory.
 * Returns 0 on success, -1 if not found.
 */
int pkg_scanner_set_blurhash(const char *path, const char *blurhash);

/**
 * Searches scan directories for a specific part of a multi-part archive.
 * Matches by package UUID (if provided) and/or PKG filename, and part_index.
 * Returns 0 if found and writes path to out_path, -1 if not found.
 */
int pkg_scanner_find_part(const uint8_t *package_uuid, const char *pkg_filename,
                         uint32_t part_index, char *out_path, size_t out_max);

/**
 * Extended search that also reports if a different part of the same package was detected.
 */
int pkg_scanner_find_part_ex(const uint8_t *package_uuid, const char *pkg_filename,
                            uint32_t part_index, char *out_path, size_t out_max,
                            uint32_t *out_detected_part);

typedef struct {
    char id[32];          /* "usb0", "usb1", ..., "disc", "internal" */
    char label[64];       /* "USB Drive 0", "Blu-ray Disc", "Internal Storage" */
    char path[256];       /* "/mnt/usb0", "/mnt/disc", "/data/pkg" */
    char type[16];        /* "usb", "disc", "internal" */
    int mounted;          /* 1 if mounted, 0 otherwise */
    size_t pkg_count;     /* Number of packages in root */
    int clickable;        /* 1 if pkg_count > 0, 0 otherwise */
} pkg_drive_t;

/**
 * Returns number of detected drives.
 */
size_t pkg_scanner_get_drive_count(void);

/**
 * Retrieves drive details at index (0 <= index < drive_count).
 */
int pkg_scanner_get_drive_at(size_t index, pkg_drive_t *out);

/**
 * Serializes list of mounted drives into JSON array.
 * Caller must free() the returned buffer.
 */
char *pkg_scanner_drives_to_json(void);

/**
 * Serializes packages for a specific drive into JSON array, optionally resolving
 * display titles using accept_language header.
 * Caller must free() the returned buffer.
 */
char *pkg_scanner_packages_for_drive_to_json_ex(const char *drive_id_or_path, const char *accept_language);

/**
 * Serializes packages for a specific drive (by id e.g. "usb0" or path e.g. "/mnt/usb0") into JSON array.
 * If drive_id_or_path is NULL or empty, returns all packages.
 * Caller must free() the returned buffer.
 */
char *pkg_scanner_packages_for_drive_to_json(const char *drive_id_or_path);

/**
 * Serializes the list of scanned packages into a JSON array string.
 * The returned buffer must be freed by caller using free().
 */
char *pkg_scanner_to_json(void);

/**
 * Scan progress status for full-screen UI progress overlay.
 */
typedef struct {
    int is_scanning;
    size_t total_files;
    size_t processed_files;
    char current_drive[64];
    char current_file[256];
} pkg_scan_status_t;

void pkg_scanner_get_status(pkg_scan_status_t *out);
char *pkg_scanner_status_to_json(void);

/**
 * Performs a quick / light rescan of packages.
 * If drive_id_or_path is non-NULL and non-empty (and not "__all__"), scans only that drive.
 * Otherwise, scans all drives and enabled SMB shares.
 * Unchanged sources (matching file list, size, mtime) are skipped with 0 parsing overhead.
 * Only new/modified files are parsed. Removed files are purged.
 * If out_changed is non-NULL, sets *out_changed to 1 if packages/drives changed, 0 otherwise.
 * Returns total number of packages currently in catalog.
 */
int pkg_scanner_scan_quick(const char *drive_id_or_path, int *out_changed);

/**
 * Checks if a persisted catalog manifest exists on disk.
 * Returns 1 if manifest exists, 0 otherwise.
 */
int pkg_scanner_has_manifest(void);

#ifdef __cplusplus
}
#endif

#endif /* PKG_SCANNER_H */
