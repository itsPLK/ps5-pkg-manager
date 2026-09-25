#ifndef MULTIPART_H
#define MULTIPART_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#define MULTIPART_MAGIC "PS5MPKG1"
#define MULTIPART_MAGIC_LEN 8
#define MULTIPART_HEADER_SIZE 4096

#define MULTIPART_COMPRESSION_NONE    0
#define MULTIPART_COMPRESSION_DEFLATE 1

#define MAX_MULTIPART_PARTS 256

#pragma pack(push, 1)
typedef struct {
    char magic[8];                  /* "PS5MPKG1" */
    uint32_t header_version;        /* 1 */
    uint32_t part_index;            /* 1-indexed (1, 2, ...) */
    uint32_t total_parts;           /* Total number of parts (e.g. 2, 3) */
    uint32_t compression_type;      /* 0 = none/raw slice, 1 = deflate/zlib (deprecated) */
    uint32_t chunk_size;            /* Nominal slice block size (e.g. 2MB) */
    uint32_t num_chunks_in_part;    /* Number of chunks in part (0 for raw uncompressed slice) */
    uint64_t part_data_size;        /* Byte size of raw PKG slice in this part */
    uint64_t total_pkg_size;        /* Total uncompressed PKG file size across all parts */
    uint64_t total_archive_size;    /* Combined size of all part files */
    char pkg_filename[256];         /* Original PKG filename (e.g. "Title.pkg") */
    char title_id[32];              /* Extracted Title ID (e.g. "PPSA01650") */
    char title_name[256];         /* Extracted Title Name (e.g. "BounceQuest") */
    char content_id[64];            /* Extracted Content ID */
    uint8_t package_uuid[16];       /* Unique identifier shared across all parts */
    uint32_t icon_offset;           /* Byte offset of icon0.png in this file (0 if none) */
    uint32_t icon_size;             /* Byte size of icon0.png */
    char app_version[32];           /* Extracted Application Version */
    char pkg_type[16];              /* "base", "update", "dlc" */
    uint64_t part_offset;           /* Byte offset in original PKG where this part slice begins */
    uint32_t data_offset;           /* Byte offset where raw PKG slice begins in this file */
    uint8_t reserved[3348];         /* Padded to 4096 bytes */
} multipart_header_t;

typedef struct {
    uint32_t uncompressed_size;     /* Uncompressed size of this chunk */
    uint32_t compressed_size;       /* Compressed size stored on disk */
    uint32_t chunk_crc32;           /* CRC32 of uncompressed chunk data */
} multipart_chunk_header_t;
#pragma pack(pop)

typedef void (*multipart_progress_fn)(uint64_t bytes_processed, uint64_t total_bytes, void *user_data);

typedef struct {
    char path[512];
    uint64_t start_pkg_offset;
    uint64_t part_data_size;
    uint32_t data_offset;
    int fd;
} virtual_part_info_t;

typedef struct {
    int is_multipart;
    int is_smb;
    int is_live;           /* NEW: RAM live session (ws_stream.c), no file */
    void *live;            /* ws_stream session handle (global singleton) */
    void *smb_session;
    uint32_t current_part;
    uint32_t total_parts;
    uint64_t total_pkg_size;
    char pkg_filename[256];
    char title_id[32];
    char title_name[256];
    uint8_t package_uuid[16];
    virtual_part_info_t parts[MAX_MULTIPART_PARTS];
    /* Guards part lookup/open and current_part bookkeeping so the actual
     * data transfer (pread / smb session, which self-serializes) can run
     * without holding the caller's lock. */
    pthread_mutex_t lock;
    int lock_inited;
} virtual_stream_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reads and validates a multi-part archive header from the given file path.
 * Returns 0 on success, or negative error code.
 */
int multipart_read_header(const char *file_path, multipart_header_t *out_hdr);

/**
 * Validates whether the given in-memory header struct has a valid signature and fields.
 * Returns 1 if valid, 0 otherwise.
 */
int multipart_is_valid_header(const multipart_header_t *hdr);

/**
 * Checks if a filename matches a multi-part file extension case-insensitively
 * (e.g. .pkg.part2, .part2, .pkg.PART2, .part02, .part2.pkg, part_2.pkg.part).
 * If target_part > 0, returns 1 only if the extracted part index equals target_part.
 * If target_part == 0, matches any part and writes the extracted part number to out_part_num if non-NULL.
 * Returns 1 on match, 0 otherwise.
 */
int multipart_is_part_filename(const char *filename, uint32_t target_part, uint32_t *out_part_num);

/**
 * Assembles and decompresses all parts in order into the target PKG file.
 * part_paths is an array of pointers to file paths for parts 1 through num_parts.
 * Reports progress through progress_cb if provided.
 * cancel_flag can be set non-zero to abort decompression midway.
 * Returns 0 on success, negative error code on failure.
 */
int multipart_decompress_to_pkg(const char **part_paths,
                                uint32_t num_parts,
                                const char *out_pkg_path,
                                multipart_progress_fn progress_cb,
                                void *user_data,
                                volatile int *cancel_flag);

typedef int (*virtual_stream_find_part_fn)(const uint8_t *package_uuid, const char *pkg_filename,
                                          uint32_t part_index, char *out_path, size_t out_max);
void virtual_stream_set_part_finder(virtual_stream_find_part_fn fn);

typedef int (*virtual_stream_wait_disc_fn)(const uint8_t *package_uuid, const char *pkg_filename,
                                          uint32_t part_index, uint32_t total_parts,
                                          char *out_path, size_t out_max);
void virtual_stream_set_disc_waiter(virtual_stream_wait_disc_fn fn);

typedef void (*virtual_stream_part_notify_fn)(uint32_t current_part, uint32_t total_parts);
void virtual_stream_set_part_notifier(virtual_stream_part_notify_fn fn);

/**
 * Virtual Stream APIs for on-the-fly streaming of single/multi-part packages.
 */
int virtual_stream_open(const char *initial_path, virtual_stream_t *stream);
ssize_t virtual_stream_read(virtual_stream_t *stream, uint64_t pkg_offset, void *buf, size_t count);
void virtual_stream_close(virtual_stream_t *stream);

/**
 * For SMB streams: returns the smb:// URL of the backing file so the caller
 * can open its own private SMB session for parallel reads.  Returns NULL for
 * non-SMB streams.
 */
const char *virtual_stream_get_smb_url(const virtual_stream_t *stream);

/**
 * Checks if a package source path exists and is accessible (local file, SMB, or live).
 * Returns 0 if accessible, negative if missing or inaccessible.
 */
int virtual_stream_check_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* MULTIPART_H */
