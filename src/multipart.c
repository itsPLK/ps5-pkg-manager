/*
 * PKG Manager - Multi-Part Package Format & Virtual Stream
 *
 * Implements PS5MPKG1 binary slice format validation, multi-disc
 * part resolution, and virtual stream random-access reads.
 */

#include "multipart.h"
#include "miniz.h"
#include "smb_client.h"
#include "installer.h"
#include "ws_stream.h" /* NEW: live: scheme backend (additive; no SMB/file paths touched) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

int multipart_is_valid_header(const multipart_header_t *hdr) {
    if (!hdr) return 0;
    if (memcmp(hdr->magic, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN) != 0) {
        return 0;
    }
    if (hdr->header_version != 1) {
        return 0;
    }
    if (hdr->total_parts == 0 || hdr->part_index == 0 || hdr->part_index > hdr->total_parts) {
        return 0;
    }
    if (hdr->compression_type != MULTIPART_COMPRESSION_NONE &&
        hdr->compression_type != MULTIPART_COMPRESSION_DEFLATE) {
        return 0;
    }
    if (hdr->chunk_size > 64 * 1024 * 1024) {
        return 0;
    }
    return 1;
}

int multipart_is_part_filename(const char *filename, uint32_t target_part, uint32_t *out_part_num) {
    if (!filename || filename[0] == '\0') return 0;

    const char *slash = strrchr(filename, '/');
    const char *name = slash ? slash + 1 : filename;
    if (name[0] == '.' || name[0] == '\0') return 0;

    /* Staged temporary files: part_%u.pkg.part */
    if (strncasecmp(name, "part_", 5) == 0) {
        char *endptr = NULL;
        unsigned long n = strtoul(name + 5, &endptr, 10);
        if (n > 0 && n <= MAX_MULTIPART_PARTS && endptr && strcasecmp(endptr, ".pkg.part") == 0) {
            if (out_part_num) *out_part_num = (uint32_t)n;
            return (target_part == 0 || target_part == (uint32_t)n);
        }
    }

    /* Look for ".part" (case-insensitive, e.g. .pkg.part2, .part2, .pkg.PART2, .part02).
     * Use the LAST occurrence so "my.part1.pkg.part2" resolves to part 2. */
    const char *p = NULL;
    const char *cursor = name;
    while ((cursor = strcasestr(cursor, ".part")) != NULL) {
        /* Require at least one digit after ".part" to be a candidate */
        if (cursor[5] >= '0' && cursor[5] <= '9') {
            p = cursor;
        }
        cursor += 5;
    }
    if (!p) {
        return 0;
    }

    /* Advance past ".part" */
    p += 5;
    if (*p == '\0') return 0;

    char *endptr = NULL;
    unsigned long n = strtoul(p, &endptr, 10);
    if (n == 0 || n > MAX_MULTIPART_PARTS) return 0;

    /* If there are characters after the number, allow only trailing ".pkg" (e.g. .part2.pkg) */
    if (endptr && *endptr != '\0' && strcasecmp(endptr, ".pkg") != 0) {
        return 0;
    }

    if (out_part_num) *out_part_num = (uint32_t)n;
    if (target_part == 0) return 1;
    return (target_part == (uint32_t)n);
}

int multipart_read_header(const char *file_path, multipart_header_t *out_hdr) {
    if (!file_path || !out_hdr) {
        return -1;
    }

    if (strncmp(file_path, "smb://", 6) == 0) {
        return -1; /* Multi-part format is only supported on local drives (USB / optical discs) */
    }

    const char *p = (strncmp(file_path, "file://", 7) == 0) ? file_path + 7 : file_path;
    FILE *f = fopen(p, "rb");
    if (!f) {
        return -1;
    }

    size_t read_bytes = fread(out_hdr, 1, sizeof(multipart_header_t), f);
    fclose(f);

    if (read_bytes < sizeof(multipart_header_t)) {
        return -1;
    }

    if (!multipart_is_valid_header(out_hdr)) {
        return -2;
    }

    return 0;
}

int multipart_decompress_to_pkg(const char **part_paths,
                                uint32_t num_parts,
                                const char *out_pkg_path,
                                multipart_progress_fn progress_cb,
                                void *user_data,
                                volatile int *cancel_flag) {
    if (!part_paths || num_parts == 0 || !out_pkg_path) {
        return -1;
    }

    /* 1. Read and validate all part headers upfront */
    multipart_header_t first_hdr;
    if (multipart_read_header(part_paths[0], &first_hdr) != 0) {
        return -2;
    }

    if (first_hdr.total_parts != num_parts) {
        return -3; /* Provided number of parts does not match header */
    }

    uint8_t expected_uuid[16];
    memcpy(expected_uuid, first_hdr.package_uuid, 16);
    uint32_t chunk_size = first_hdr.chunk_size;
    if (chunk_size == 0) {
        chunk_size = 2 * 1024 * 1024; /* 2MB default */
    }

    /* Verify all parts and accumulate total slice sizes */
    uint64_t parts_sum = first_hdr.part_data_size;
    for (uint32_t i = 1; i < num_parts; i++) {
        multipart_header_t part_hdr;
        if (multipart_read_header(part_paths[i], &part_hdr) != 0) {
            return -4;
        }
        if (part_hdr.part_index != (i + 1) || part_hdr.total_parts != num_parts) {
            return -5;
        }
        if (memcmp(part_hdr.package_uuid, expected_uuid, 16) != 0) {
            return -6; /* UUID mismatch between parts */
        }
        parts_sum += part_hdr.part_data_size;
    }

    /* Pre-validate total sizes before allocating or creating output file:
     * fail fast instead of filling /data and detecting mismatch at the end. */
    if (first_hdr.total_pkg_size > 0 && parts_sum != 0 &&
        parts_sum != first_hdr.total_pkg_size) {
        return -18; /* Size mismatch between part slices and total PKG size */
    }

    /* 2. Open destination PKG file */
    FILE *out_f = fopen(out_pkg_path, "wb");
    if (!out_f) {
        return -7;
    }

    /* Fixed 2MB streaming buffer for raw slices: header chunk_size (up to
     * 64MB) must not dictate ~192MB mallocs on a payload heap. Chunked
     * legacy path still needs header-sized buffers, validated below. */
    size_t raw_buf_cap = 2 * 1024 * 1024;
    size_t uncomp_buf_cap = raw_buf_cap;
    size_t comp_buf_cap = raw_buf_cap;
    int need_chunked = (first_hdr.num_chunks_in_part != 0 ||
                        first_hdr.compression_type != MULTIPART_COMPRESSION_NONE);
    if (need_chunked) {
        if (chunk_size > 8 * 1024 * 1024) {
            fclose(out_f);
            unlink(out_pkg_path);
            return -8;
        }
        uncomp_buf_cap = (size_t)chunk_size + 65536;
        comp_buf_cap = (size_t)chunk_size * 2 + 65536;
    }

    uint8_t *uncomp_buf = (uint8_t *)malloc(uncomp_buf_cap);
    uint8_t *comp_buf = (uint8_t *)malloc(comp_buf_cap);

    if (!uncomp_buf || !comp_buf) {
        if (uncomp_buf) free(uncomp_buf);
        if (comp_buf) free(comp_buf);
        fclose(out_f);
        unlink(out_pkg_path);
        return -8;
    }

    uint64_t total_written = 0;
    uint64_t total_expected = first_hdr.total_pkg_size;
    int err = 0;

    /* 3. Process each part */
    for (uint32_t p = 0; p < num_parts; p++) {
        if (cancel_flag && *cancel_flag) {
            err = -99;
            break;
        }

        FILE *in_f = fopen(part_paths[p], "rb");
        if (!in_f) {
            err = -9;
            break;
        }

        multipart_header_t part_hdr;
        if (fread(&part_hdr, 1, sizeof(part_hdr), in_f) != sizeof(part_hdr)) {
            fclose(in_f);
            err = -10;
            break;
        }

        /* Seek past any embedded icon data right after header.
         * Validate offsets: crafted data_offset must not seek past EOF. */
        uint64_t start_payload_offset = part_hdr.data_offset;
        if (start_payload_offset == 0) {
            start_payload_offset = MULTIPART_HEADER_SIZE;
            if (part_hdr.icon_size > 0 && part_hdr.icon_offset == MULTIPART_HEADER_SIZE) {
                if (part_hdr.icon_size >= 10 * 1024 * 1024) {
                    fclose(in_f);
                    err = -10;
                    break;
                }
                start_payload_offset += part_hdr.icon_size;
            }
        }
        if (fseek(in_f, (long)start_payload_offset, SEEK_SET) != 0) {
            fclose(in_f);
            err = -10;
            break;
        }

        if (part_hdr.num_chunks_in_part == 0 && part_hdr.compression_type == MULTIPART_COMPRESSION_NONE) {
            /* Raw uncompressed sequential slice */
            uint64_t bytes_left = part_hdr.part_data_size;
            while (bytes_left > 0) {
                if (cancel_flag && *cancel_flag) {
                    err = -99;
                    break;
                }
                size_t to_read = (bytes_left < uncomp_buf_cap) ? (size_t)bytes_left : uncomp_buf_cap;
                size_t n = fread(uncomp_buf, 1, to_read, in_f);
                if (n == 0) {
                    err = -13;
                    break;
                }
                if (fwrite(uncomp_buf, 1, n, out_f) != n) {
                    err = -14;
                    break;
                }
                total_written += n;
                bytes_left -= n;
                if (progress_cb) {
                    progress_cb(total_written, total_expected, user_data);
                }
            }
        } else {
            /* Legacy chunked format */
            for (uint32_t c = 0; c < part_hdr.num_chunks_in_part; c++) {
                if (cancel_flag && *cancel_flag) {
                    err = -99;
                    break;
                }

                multipart_chunk_header_t chdr;
                if (fread(&chdr, 1, sizeof(chdr), in_f) != sizeof(chdr)) {
                    err = -11;
                    break;
                }

                if (chdr.compressed_size > comp_buf_cap || chdr.uncompressed_size > uncomp_buf_cap) {
                    err = -12;
                    break;
                }

                if (fread(comp_buf, 1, chdr.compressed_size, in_f) != chdr.compressed_size) {
                    err = -13;
                    break;
                }

                if (part_hdr.compression_type == MULTIPART_COMPRESSION_NONE) {
                    /* Stored / uncompressed: uncompressed_size must equal compressed_size */
                    if (chdr.uncompressed_size != chdr.compressed_size) {
                        err = -12;
                        break;
                    }
                    /* Verify CRC if stored */
                    if (chdr.chunk_crc32 != 0) {
                        uint32_t calc_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, comp_buf, chdr.uncompressed_size);
                        if (calc_crc != chdr.chunk_crc32) {
                            err = -16; /* CRC check failure */
                            break;
                        }
                    }
                    if (fwrite(comp_buf, 1, chdr.uncompressed_size, out_f) != chdr.uncompressed_size) {
                        err = -14;
                        break;
                    }
                    total_written += chdr.uncompressed_size;
                } else if (part_hdr.compression_type == MULTIPART_COMPRESSION_DEFLATE) {
                    /* Deflate compressed */
                    mz_ulong dest_len = (mz_ulong)uncomp_buf_cap;
                    int z_ret = mz_uncompress(uncomp_buf, &dest_len, comp_buf, (mz_ulong)chdr.compressed_size);
                    if (z_ret != MZ_OK || dest_len != (mz_ulong)chdr.uncompressed_size) {
                        err = -15;
                        break;
                    }

                    /* Check CRC if stored */
                    if (chdr.chunk_crc32 != 0) {
                        uint32_t calc_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, uncomp_buf, dest_len);
                        if (calc_crc != chdr.chunk_crc32) {
                            err = -16; /* CRC check failure */
                            break;
                        }
                    }

                    if (fwrite(uncomp_buf, 1, dest_len, out_f) != dest_len) {
                        err = -17;
                        break;
                    }
                    total_written += dest_len;
                }

                if (progress_cb) {
                    progress_cb(total_written, total_expected, user_data);
                }
            }
        }

        fclose(in_f);
        if (err != 0) break;
    }

    free(uncomp_buf);
    free(comp_buf);
    fclose(out_f);

    if (err != 0) {
        unlink(out_pkg_path);
        return err;
    }

    /* Verify total uncompressed bytes written matches expected total */
    if (total_expected > 0 && total_written != total_expected) {
        unlink(out_pkg_path);
        return -18; /* Incomplete decompression or size mismatch */
    }

    return 0;
}

static virtual_stream_find_part_fn g_stream_find_part_fn = NULL;
static virtual_stream_wait_disc_fn g_stream_wait_disc_fn = NULL;
static virtual_stream_part_notify_fn g_stream_part_notify_fn = NULL;

void virtual_stream_set_part_finder(virtual_stream_find_part_fn fn) {
    g_stream_find_part_fn = fn;
}

void virtual_stream_set_disc_waiter(virtual_stream_wait_disc_fn fn) {
    g_stream_wait_disc_fn = fn;
}

void virtual_stream_set_part_notifier(virtual_stream_part_notify_fn fn) {
    g_stream_part_notify_fn = fn;
}

int virtual_stream_open(const char *initial_path, virtual_stream_t *stream) {
    if (!initial_path || !stream) return -1;
    _Static_assert(sizeof(multipart_header_t) == MULTIPART_HEADER_SIZE,
                   "multipart_header_t must be exactly 4096 bytes");
    memset(stream, 0, sizeof(*stream));

    for (int i = 0; i < MAX_MULTIPART_PARTS; i++) {
        stream->parts[i].fd = -1;
    }
    pthread_mutex_init(&stream->lock, NULL);
    stream->lock_inited = 1;

    /* NEW: live RAM session pushed from a browser (Direct Install).
     * Attaches to the current ws_live session when the id matches;
     * works while uploading and after byte-complete. */
    if (strncmp(initial_path, "live:", 5) == 0) {
        const char *id = initial_path + 5;
        if (!ws_live_check_id(id)) {
            virtual_stream_close(stream);
            return -1;
        }
        uint64_t ltotal = ws_live_get_total();
        if (ltotal == 0) {
            virtual_stream_close(stream);
            return -1;
        }
        stream->is_multipart = 0;
        stream->is_smb = 0;
        stream->is_live = 1;
        stream->live = (void *)1; /* singleton handle; lifecycle owned by ws layer */
        stream->current_part = 1;
        stream->total_parts = 1;
        stream->total_pkg_size = ltotal;
        snprintf(stream->pkg_filename, sizeof(stream->pkg_filename), "%s.pkg", id);
        if (ws_live_attach() != 0) {
            stream->is_live = 0;
            stream->live = NULL;
            virtual_stream_close(stream);
            return -1;
        }
        return 0;
    }

    if (strncmp(initial_path, "smb://", 6) == 0) {
        smb_file_session_t *sess = smb_file_session_open(initial_path);
        if (!sess) {
            virtual_stream_close(stream);
            return -1;
        }
        uint64_t fsz = smb_file_session_get_size(sess);
        stream->is_multipart = 0;
        stream->is_smb = 1;
        stream->smb_session = (void *)sess;
        stream->current_part = 1;
        stream->total_parts = 1;
        stream->total_pkg_size = fsz;
        strncpy(stream->parts[0].path, initial_path, sizeof(stream->parts[0].path) - 1);
        stream->parts[0].start_pkg_offset = 0;
        stream->parts[0].part_data_size = fsz;
        stream->parts[0].data_offset = 0;
        stream->parts[0].fd = -1;
        const char *slash = strrchr(initial_path, '/');
        strncpy(stream->pkg_filename, slash ? slash + 1 : initial_path, sizeof(stream->pkg_filename) - 1);
        return 0;
    }

    const char *local_path = (strncmp(initial_path, "file://", 7) == 0) ? initial_path + 7 : initial_path;
    struct stat st;
    if (stat(local_path, &st) != 0) {
        virtual_stream_close(stream);
        return -1;
    }

    multipart_header_t hdr;
    if (multipart_read_header(local_path, &hdr) != 0) {
        /* Standard single PKG file */
        stream->is_multipart = 0;
        stream->is_smb = 0;
        stream->current_part = 1;
        stream->total_parts = 1;
        stream->total_pkg_size = (uint64_t)st.st_size;
        strncpy(stream->parts[0].path, local_path, sizeof(stream->parts[0].path) - 1);
        stream->parts[0].start_pkg_offset = 0;
        stream->parts[0].part_data_size = stream->total_pkg_size;
        stream->parts[0].data_offset = 0;
        stream->parts[0].fd = -1;
        const char *slash = strrchr(local_path, '/');
        strncpy(stream->pkg_filename, slash ? slash + 1 : local_path, sizeof(stream->pkg_filename) - 1);
        return 0;
    }

    /* Multi-part package (local drive / optical disc only).
     * Disk strings may lack NUL: clamp before use. */
    hdr.pkg_filename[sizeof(hdr.pkg_filename) - 1] = '\0';
    hdr.title_id[sizeof(hdr.title_id) - 1] = '\0';
    hdr.title_name[sizeof(hdr.title_name) - 1] = '\0';
    stream->is_multipart = 1;
    stream->is_smb = 0;
    stream->current_part = 1;
    stream->total_parts = hdr.total_parts;
    stream->total_pkg_size = hdr.total_pkg_size;
    strncpy(stream->pkg_filename, hdr.pkg_filename, sizeof(stream->pkg_filename) - 1);
    stream->pkg_filename[sizeof(stream->pkg_filename) - 1] = '\0';
    strncpy(stream->title_id, hdr.title_id, sizeof(stream->title_id) - 1);
    stream->title_id[sizeof(stream->title_id) - 1] = '\0';
    strncpy(stream->title_name, hdr.title_name, sizeof(stream->title_name) - 1);
    stream->title_name[sizeof(stream->title_name) - 1] = '\0';
    memcpy(stream->package_uuid, hdr.package_uuid, 16);

    if (stream->total_parts == 0 || stream->total_parts > MAX_MULTIPART_PARTS) {
        virtual_stream_close(stream);
        return -2;
    }

    /* Resolve all parts */
    for (uint32_t p = 1; p <= stream->total_parts; p++) {
        char part_path[512] = {0};
        if (p == hdr.part_index) {
            strncpy(part_path, initial_path, sizeof(part_path) - 1);
        } else {
            int found = 0;
            if (g_stream_find_part_fn &&
                g_stream_find_part_fn(hdr.package_uuid, hdr.pkg_filename, p, part_path, sizeof(part_path)) == 0) {
                found = 1;
            }
            if (!found) {
                /* Try finding adjacent files in same directory (case-insensitive) */
                char dir[512];
                strncpy(dir, initial_path, sizeof(dir) - 1);
                char *slash = strrchr(dir, '/');
                if (slash) {
                    *slash = '\0';
                    DIR *adj_d = opendir(dir);
                    if (adj_d) {
                        struct dirent *de;
                        while ((de = readdir(adj_d)) != NULL) {
                            if (de->d_name[0] == '.') continue;
                            if (multipart_is_part_filename(de->d_name, p, NULL)) {
                                char cand[1024];
                                snprintf(cand, sizeof(cand), "%s/%s", dir, de->d_name);
                                multipart_header_t chdr;
                                if (multipart_read_header(cand, &chdr) == 0) {
                                    if (memcmp(chdr.package_uuid, hdr.package_uuid, 16) == 0 && chdr.part_index == p) {
                                        strncpy(part_path, cand, sizeof(part_path) - 1);
                                        found = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        closedir(adj_d);
                    }
                }
            }
            if (!found) {
                /* Try PKG_TMP_DIR */
                const char *tmp_dir = getenv("PKG_TMP_DIR");
                if (!tmp_dir || tmp_dir[0] == '\0') tmp_dir = "/data/pkgmgr/tmp";
                char cand[512];
                snprintf(cand, sizeof(cand), "%s/part_%u.pkg.part", tmp_dir, p);
                struct stat st_cand;
                if (stat(cand, &st_cand) == 0) {
                    strncpy(part_path, cand, sizeof(part_path) - 1);
                    found = 1;
                }
            }
            if (!found) {
                /* Part p is not currently mounted (e.g. on another optical disc).
                 * Do not fail open: defer resolving Part p until read time when swapped. */
                stream->parts[p - 1].path[0] = '\0';
                stream->parts[p - 1].fd = -1;
                stream->parts[p - 1].data_offset = MULTIPART_HEADER_SIZE;
                if (p > 1) {
                    stream->parts[p - 1].start_pkg_offset =
                        stream->parts[p - 2].start_pkg_offset + stream->parts[p - 2].part_data_size;
                }
                if (p == stream->total_parts) {
                    stream->parts[p - 1].part_data_size =
                        stream->total_pkg_size - stream->parts[p - 1].start_pkg_offset;
                } else {
                    stream->parts[p - 1].part_data_size = hdr.part_data_size;
                }
                continue;
            }
        }

        multipart_header_t phdr;
        if (multipart_read_header(part_path, &phdr) != 0) {
            virtual_stream_close(stream);
            return -4;
        }

        strncpy(stream->parts[p - 1].path, part_path, sizeof(stream->parts[p - 1].path) - 1);
        stream->parts[p - 1].fd = -1;

        uint32_t doff = phdr.data_offset;
        if (doff == 0) {
            doff = MULTIPART_HEADER_SIZE;
            if (phdr.part_index == 1 && phdr.icon_size > 0 && phdr.icon_offset == MULTIPART_HEADER_SIZE) {
                doff += phdr.icon_size;
            }
        }
        stream->parts[p - 1].data_offset = doff;
        stream->parts[p - 1].part_data_size = phdr.part_data_size;

        if (p == 1) {
            stream->parts[0].start_pkg_offset = 0;
        } else {
            if (phdr.part_offset > 0) {
                stream->parts[p - 1].start_pkg_offset = phdr.part_offset;
            } else {
                stream->parts[p - 1].start_pkg_offset = stream->parts[p - 2].start_pkg_offset + stream->parts[p - 2].part_data_size;
            }
        }
    }

    return 0;
}

ssize_t virtual_stream_read(virtual_stream_t *stream, uint64_t pkg_offset, void *buf, size_t count) {
    if (!stream || !buf || count == 0) return 0;
    if (pkg_offset >= stream->total_pkg_size) return 0;

    /* NEW: live RAM session. Delivers full count or fails (0 at true EOF,
     * -1 on abort/timeout/evicted range); never returns partial mid-file
     * data, matching what the stream server expects. */
    if (stream->is_live) {
        size_t to_read = count;
        if (pkg_offset + to_read > stream->total_pkg_size) {
            to_read = (size_t)(stream->total_pkg_size - pkg_offset);
        }
        if (to_read == 0) return 0;
        long n = ws_live_read(pkg_offset, buf, to_read);
        if (n < 0) return -1;
        return (ssize_t)n;
    }

    if (stream->is_smb && stream->smb_session) {
        /* SMB sessions self-serialize via their own mutex; no stream
         * lock is taken here so slow network reads never stall the
         * caller's lock. */
        size_t to_read = count;
        if (pkg_offset + to_read > stream->total_pkg_size) {
            to_read = (size_t)(stream->total_pkg_size - pkg_offset);
        }
        return smb_file_session_read((smb_file_session_t *)stream->smb_session, buf, to_read, pkg_offset);
    }

    /* Local path: resolve the backing fd + file range under the stream
     * lock, then transfer outside it so parallel range connections and
     * the (potentially hour-long) disc wait never block each other. */
    int use_lock = stream->lock_inited;
    for (;;) {
        int fd = -1;
        uint64_t file_offset_u64 = 0;
        size_t chunk = 0;
        uint8_t wait_uuid[16];
        char wait_filename[256] = {0};
        uint32_t wait_part = 0;
        uint32_t wait_total = 0;
        int need_wait = 0;

        if (use_lock) pthread_mutex_lock(&stream->lock);
        size_t to_read = count;
        if (pkg_offset + to_read > stream->total_pkg_size) {
            to_read = (size_t)(stream->total_pkg_size - pkg_offset);
        }

        int p_idx = -1;
        for (uint32_t i = 0; i < stream->total_parts; i++) {
            uint64_t p_start = stream->parts[i].start_pkg_offset;
            uint64_t p_size = stream->parts[i].part_data_size;
            if (pkg_offset >= p_start && pkg_offset < (p_start + p_size)) {
                p_idx = (int)i;
                break;
            }
        }

        if (p_idx < 0) {
            if (use_lock) pthread_mutex_unlock(&stream->lock);
            return -1;
        }

        uint32_t cur_part = (uint32_t)(p_idx + 1);
        if (stream->current_part != cur_part) {
            stream->current_part = cur_part;
            if (g_stream_part_notify_fn) {
                g_stream_part_notify_fn(cur_part, stream->total_parts);
            }
        }

        /* Ensure part file is open, or defer to the disc waiter below */
        if (stream->parts[p_idx].path[0] == '\0' || stream->parts[p_idx].fd < 0) {
            if (stream->parts[p_idx].path[0] != '\0' && stream->parts[p_idx].fd < 0) {
                stream->parts[p_idx].fd = open(stream->parts[p_idx].path, O_RDONLY);
            }

            if (stream->parts[p_idx].fd < 0) {
                if (!g_stream_wait_disc_fn) {
                    if (use_lock) pthread_mutex_unlock(&stream->lock);
                    return -1;
                }
                memcpy(wait_uuid, stream->package_uuid, sizeof(wait_uuid));
                strncpy(wait_filename, stream->pkg_filename, sizeof(wait_filename) - 1);
                wait_part = (uint32_t)(p_idx + 1);
                wait_total = stream->total_parts;
                need_wait = 1;
                if (use_lock) pthread_mutex_unlock(&stream->lock);
            }
        }

        if (need_wait) {
            /* Blocking wait runs with NO stream lock held. */
            char found_path[512] = {0};
            int w_res = g_stream_wait_disc_fn(wait_uuid, wait_filename, wait_part, wait_total,
                                              found_path, sizeof(found_path));
            if (w_res != 0) {
                return -1;
            }

            if (use_lock) pthread_mutex_lock(&stream->lock);
            /* Close previous disc fds if any were open */
            for (uint32_t k = 0; k < stream->total_parts; k++) {
                if ((int)k != (int)(wait_part - 1) && stream->parts[k].fd >= 0) {
                    close(stream->parts[k].fd);
                    stream->parts[k].fd = -1;
                }
            }

            multipart_header_t part_hdr;
            if (multipart_read_header(found_path, &part_hdr) != 0) {
                if (use_lock) pthread_mutex_unlock(&stream->lock);
                return -1;
            }

            uint32_t widx = wait_part - 1;
            strncpy(stream->parts[widx].path, found_path, sizeof(stream->parts[widx].path) - 1);
            uint32_t doff = part_hdr.data_offset;
            if (doff == 0) {
                doff = MULTIPART_HEADER_SIZE;
                if (part_hdr.part_index == 1 && part_hdr.icon_size > 0 && part_hdr.icon_offset == MULTIPART_HEADER_SIZE) {
                    doff += part_hdr.icon_size;
                }
            }
            stream->parts[widx].data_offset = doff;
            stream->parts[widx].part_data_size = part_hdr.part_data_size;
            if (part_hdr.part_offset > 0) {
                stream->parts[widx].start_pkg_offset = part_hdr.part_offset;
            }
            stream->parts[widx].fd = open(found_path, O_RDONLY);
            if (stream->parts[widx].fd < 0) {
                if (use_lock) pthread_mutex_unlock(&stream->lock);
                return -1;
            }
            if (use_lock) pthread_mutex_unlock(&stream->lock);
            continue; /* re-resolve under lock with the fresh part */
        }

        uint64_t offset_in_part;
        if (pkg_offset < stream->parts[p_idx].start_pkg_offset) {
            if (use_lock) pthread_mutex_unlock(&stream->lock);
            return -1;
        }
        offset_in_part = pkg_offset - stream->parts[p_idx].start_pkg_offset;
        if (offset_in_part >= stream->parts[p_idx].part_data_size) {
            if (use_lock) pthread_mutex_unlock(&stream->lock);
            return -1;
        }
        uint64_t remaining_in_part = stream->parts[p_idx].part_data_size - offset_in_part;
        chunk = (to_read < remaining_in_part) ? to_read : (size_t)remaining_in_part;
        if (chunk == 0) {
            if (use_lock) pthread_mutex_unlock(&stream->lock);
            return 0;
        }

        fd = stream->parts[p_idx].fd;
        file_offset_u64 = (uint64_t)stream->parts[p_idx].data_offset + offset_in_part;
        if (use_lock) pthread_mutex_unlock(&stream->lock);

        /* Loop pread to handle short reads/EINTR; truncated parts are errors
         * (return -1) instead of silent short success that corrupts installs. */
        size_t done = 0;
        while (done < chunk) {
            ssize_t n = pread(fd, (char *)buf + done, chunk - done,
                              (off_t)file_offset_u64 + (off_t)done);
            if (n < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (n == 0) {
                return -1;
            }
            done += (size_t)n;
        }
        return (ssize_t)done;
    }
}

void virtual_stream_close(virtual_stream_t *stream) {
    if (!stream) return;
    /* NEW: live sessions are owned by the ws layer (abort/destroy there);
     * close only detaches so session_stop never blocks on readers (the
     * abort that unblocks them runs before stop drains vs_refs). */
    if (stream->is_live) {
        stream->is_live = 0;
        stream->live = NULL;
        ws_live_detach();
    }
    if (stream->is_smb && stream->smb_session) {
        smb_file_session_close((smb_file_session_t *)stream->smb_session);
        stream->smb_session = NULL;
    }
    for (uint32_t i = 0; i < stream->total_parts && i < MAX_MULTIPART_PARTS; i++) {
        if (stream->parts[i].fd >= 0) {
            close(stream->parts[i].fd);
            stream->parts[i].fd = -1;
        }
    }
    if (stream->lock_inited) {
        pthread_mutex_destroy(&stream->lock);
        stream->lock_inited = 0;
    }
}

const char *virtual_stream_get_smb_url(const virtual_stream_t *stream) {
    if (!stream || !stream->is_smb) return NULL;
    if (stream->parts[0].path[0] == '\0') return NULL;
    return stream->parts[0].path;
}

int virtual_stream_check_path(const char *path) {
    if (!path || path[0] == '\0') return -1;
    if (strncmp(path, "live:", 5) == 0) {
        return ws_live_check_id(path + 5) ? 0 : -1;
    }
    if (strncmp(path, "smb://", 6) == 0) {
        smb_file_session_t *sess = smb_file_session_open(path);
        if (!sess) return -1;
        smb_file_session_close(sess);
        return 0;
    }
    const char *local_path = (strncmp(path, "file://", 7) == 0) ? path + 7 : path;
    struct stat st;
    return stat(local_path, &st) == 0 ? 0 : -1;
}
