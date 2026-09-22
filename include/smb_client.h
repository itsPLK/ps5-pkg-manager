#ifndef SMB_CLIENT_H
#define SMB_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "pkg_parser.h"

#define MAX_SMB_SHARES 8
#define SMB_DEFAULT_PORT 445

typedef struct {
    int enabled;
    char id[32];             /* e.g. "smb0", "smb1" */
    char label[64];          /* User label: "NAS Packages" */
    char server[128];        /* Server IP or hostname */
    int port;                /* Default 445 */
    char share[128];         /* Share name, e.g. "pkgs" */
    char path[256];          /* Subfolder within share, e.g. "" or "pkgs" */
    char username[64];       /* Username, empty for guest */
    char password[64];       /* Password */
    char workgroup[64];      /* Workgroup / domain, e.g. "WORKGROUP" */
    int is_read_only;        /* 0 = read/write (cache to share), 1 = read-only */
} smb_share_config_t;

/* Parse an smb:// URL into its components:
 * Example: smb://192.168.1.100:445/pkgs/sub/title.pkg
 * -> server="192.168.1.100", port=445, share="pkgs", rel_path="/sub/title.pkg"
 */
int smb_client_parse_url(const char *smb_url,
                         char *out_server, size_t server_sz,
                         int *out_port,
                         char *out_share, size_t share_sz,
                         char *out_path, size_t path_sz);

/* Find the matching configured SMB share config for a given smb:// URL */
int smb_client_find_share_cfg(const char *smb_url, smb_share_config_t *out_cfg);

/* Sanitize SMB share configuration (strips smb://, slashes, trims whitespace, defaults port/workgroup) */
void smb_client_sanitize_config(smb_share_config_t *cfg);

/* Test connection to an SMB share.
 * Returns 0 on success, or -1 with error message written to out_err. */
int smb_client_test_connection(const smb_share_config_t *cfg, char *out_err, size_t err_sz);

/* Browse helpers for guided setup (no manual share/path typing).
 * list_shares connects to IPC$ and enumerates visible shares.
 * list_dir lists entries inside share[/path][/subpath]. */
#define MAX_SMB_BROWSE_SHARES 128
#define MAX_SMB_BROWSE_ENTRIES 256

typedef struct {
    char name[128];
    char remark[256];
    uint32_t type;
    int is_disk;
    int is_hidden;
    int is_special; /* IPC$, ADMIN$, etc. */
} smb_share_info_t;

typedef struct {
    char name[256];
    int is_dir;
    uint64_t size;
    uint32_t mtime;
} smb_dir_entry_t;

int smb_client_list_shares(const smb_share_config_t *cfg,
                           smb_share_info_t *out, int max_out,
                           char *out_err, size_t err_sz);

int smb_client_list_dir(const smb_share_config_t *cfg, const char *subpath,
                        smb_dir_entry_t *out, int max_out,
                        char *out_err, size_t err_sz);

/* Scan an SMB share for .pkg files.
 * pkg_cb is called for each found .pkg file.
 * Returns number of packages found on success, or negative on error. */
typedef void (*smb_pkg_callback_t)(const char *smb_url, const char *filename,
                                   uint64_t file_size, uint32_t mtime,
                                   void *user_data);

int smb_client_scan_share(const smb_share_config_t *cfg,
                          smb_pkg_callback_t pkg_cb,
                          void *user_data);

/* Fast pre-scan count of .pkg files on a share (readdir only, no parsing).
 * Returns number of packages found, or negative on error. */
int smb_client_count_pkg_files(const smb_share_config_t *cfg);

/* Read random-access bytes from a remote SMB file.
 * Returns number of bytes read, or negative on error. */
ssize_t smb_client_pread(const char *smb_url, void *buf, size_t count, uint64_t offset);

/* Get file metadata (size and mtime) from SMB file */
int smb_client_stat(const char *smb_url, uint64_t *out_size, uint32_t *out_mtime);

/* Calculate checksum for an SMB package file (size + mtime + 4KB header CRC32) */
int smb_client_calc_checksum(const char *smb_url, char *out_checksum, size_t out_max);

/* Parse PKG metadata directly from an SMB package */
int smb_client_parse_pkg(const char *smb_url, pkg_detail_t *out_detail);

/* Direct icon retrieval from SMB PKG */
int smb_client_get_icon(const char *smb_url, uint8_t **out_data, size_t *out_size);

/* Opaque SMB file session for high-speed sequential or random-access streaming */
typedef struct smb_file_session smb_file_session_t;

smb_file_session_t *smb_file_session_open(const char *smb_url);
ssize_t smb_file_session_read(smb_file_session_t *session, void *buf, size_t count, uint64_t offset);
uint64_t smb_file_session_get_size(smb_file_session_t *session);
void smb_file_session_close(smb_file_session_t *session);

#endif /* SMB_CLIENT_H */
