#ifndef SPLIT_PKG_H
#define SPLIT_PKG_H

/*
 * Plain numbered piece sets, e.g. Sony's CDN layout:
 *   EP1457-PPSA04135_00-MB2BANNERLORD000_0.pkg
 *   EP1457-PPSA04135_00-MB2BANNERLORD000_1.pkg
 *   ...
 * Piece 0 is the start of the package (it begins with the usual 0x7F header
 * magic); every following piece is the next raw slice of the same file. The
 * set behaves like one big file that is the concatenation of its pieces.
 *
 * This is unrelated to the app's own PS5MPKG1 multi-part format (multipart.h).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SPLIT_PKG_MAX_PIECES 256     /* matches MAX_MULTIPART_PARTS */
#define SPLIT_PKG_MAX_PATH   500     /* longest head path we handle */

typedef struct {
    char prefix[SPLIT_PKG_MAX_PATH + 8]; /* head path without "_0.pkg" */
    char ext[8];                         /* extension as spelled on piece 0 */
    uint32_t count;                      /* contiguous pieces found (>= 2) */
    uint64_t size[SPLIT_PKG_MAX_PIECES];
    uint64_t start[SPLIT_PKG_MAX_PIECES];/* offset of each piece in the joined file */
    uint64_t total;                      /* sum of all piece sizes */
} split_pkg_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 when `path` is piece 0 of a set of two or more contiguous pieces
 * (..._0.pkg, ..._1.pkg, ...). `out` may be NULL. Local paths only. */
int split_pkg_detect(const char *path, split_pkg_t *out);

/* Returns 1 when `name` (a file in `dir`) is a later piece (_1, _2, ...) of a
 * set whose piece 0 exists in the same directory, so it must not be listed as
 * a package of its own. */
int split_pkg_is_continuation(const char *dir, const char *name);

/* Writes the path of piece `index` into out. Returns 0, or -1 if it does not fit. */
int split_pkg_piece_path(const split_pkg_t *s, uint32_t index, char *out, size_t out_max);

/* pread() over the joined file. Returns bytes read (short only at the end of
 * the set) or -1 on error before anything was read. */
ssize_t split_pkg_pread(const split_pkg_t *s, void *buf, size_t n, uint64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* SPLIT_PKG_H */
