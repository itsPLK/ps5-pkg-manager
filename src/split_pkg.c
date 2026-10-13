/*
 * PKG Manager - Plain numbered piece sets (<name>_0.pkg, <name>_1.pkg, ...)
 * See include/split_pkg.h.
 */

#include "split_pkg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Splits "<prefix>_<digits>.<ext>" where digits has no leading zero.
 * On success writes the prefix length, the piece number and the extension
 * start. Returns 1 on match. */
static int parse_piece_name(const char *name, size_t *prefix_len,
                            unsigned long *num, const char **ext) {
    size_t len = strlen(name);
    if (len < 7) return 0; /* "x_0.pkg" */
    const char *dot = name + len - 4;
    if (strcasecmp(dot, ".pkg") != 0) return 0;

    const char *end = dot; /* digits end here */
    const char *p = end;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == end) return 0;                    /* no digits */
    if (p == name || p[-1] != '_') return 0;   /* must follow '_' */
    if (p[0] == '0' && (end - p) > 1) return 0; /* no leading zeros ("_01") */
    if ((end - p) > 6) return 0;

    char digits[8];
    memcpy(digits, p, (size_t)(end - p));
    digits[end - p] = '\0';
    *num = strtoul(digits, NULL, 10);
    *prefix_len = (size_t)(p - 1 - name);
    *ext = dot;
    return 1;
}

/* Piece 0 must be a regular, non-empty file starting with the 0x7F magic. */
static int head_is_pkg(const char *path, uint64_t *size_out) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char magic = 0;
    ssize_t n = pread(fd, &magic, 1, 0);
    close(fd);
    if (n != 1 || magic != 0x7F) return 0;
    if (size_out) *size_out = (uint64_t)st.st_size;
    return 1;
}

int split_pkg_piece_path(const split_pkg_t *s, uint32_t index, char *out, size_t out_max) {
    if (!s || !out || out_max == 0) return -1;
    int n = snprintf(out, out_max, "%s_%u%s", s->prefix, (unsigned)index, s->ext);
    return (n > 0 && (size_t)n < out_max) ? 0 : -1;
}

int split_pkg_detect(const char *path, split_pkg_t *out) {
    if (!path || path[0] != '/') return 0; /* local absolute paths only */
    size_t len = strlen(path);
    if (len < 7 || len > SPLIT_PKG_MAX_PATH) return 0;

    size_t prefix_len = 0;
    unsigned long num = 0;
    const char *ext = NULL;
    if (!parse_piece_name(path, &prefix_len, &num, &ext) || num != 0) return 0;

    split_pkg_t *s = out;
    split_pkg_t local;
    if (!s) s = &local;
    memset(s, 0, sizeof(*s));
    memcpy(s->prefix, path, prefix_len);
    s->prefix[prefix_len] = '\0';
    snprintf(s->ext, sizeof(s->ext), "%s", ext);

    uint64_t head_size = 0;
    if (!head_is_pkg(path, &head_size)) return 0;
    s->size[0] = head_size;
    s->start[0] = 0;
    s->total = head_size;
    s->count = 1;

    while (s->count < SPLIT_PKG_MAX_PIECES) {
        char cand[SPLIT_PKG_MAX_PATH + 32];
        if (split_pkg_piece_path(s, s->count, cand, sizeof(cand)) != 0) break;
        struct stat st;
        if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) break;
        s->size[s->count] = (uint64_t)st.st_size;
        s->start[s->count] = s->total;
        s->total += (uint64_t)st.st_size;
        s->count++;
    }
    return s->count >= 2 ? 1 : 0;
}

int split_pkg_is_continuation(const char *dir, const char *name) {
    if (!dir || !name || name[0] == '.') return 0;
    size_t prefix_len = 0;
    unsigned long num = 0;
    const char *ext = NULL;
    if (!parse_piece_name(name, &prefix_len, &num, &ext) || num == 0) return 0;

    char head[SPLIT_PKG_MAX_PATH + 64];
    int n = snprintf(head, sizeof(head), "%s/%.*s_0%s", dir, (int)prefix_len, name, ext);
    if (n <= 0 || (size_t)n >= sizeof(head)) return 0;
    return head_is_pkg(head, NULL);
}

ssize_t split_pkg_pread(const split_pkg_t *s, void *buf, size_t n, uint64_t offset) {
    if (!s || !buf || s->count == 0) return -1;
    size_t done = 0;
    while (done < n) {
        uint64_t cur = offset + done;
        if (cur >= s->total) break;

        uint32_t idx = 0;
        while (idx + 1 < s->count && cur >= s->start[idx + 1]) idx++;

        uint64_t in_piece = cur - s->start[idx];
        uint64_t room = s->size[idx] - in_piece;
        size_t want = n - done;
        if ((uint64_t)want > room) want = (size_t)room;

        char path[SPLIT_PKG_MAX_PATH + 32];
        if (split_pkg_piece_path(s, idx, path, sizeof(path)) != 0) return done ? (ssize_t)done : -1;
        int fd = open(path, O_RDONLY);
        if (fd < 0) return done ? (ssize_t)done : -1;

        size_t got = 0;
        while (got < want) {
            ssize_t r = pread(fd, (char *)buf + done + got, want - got, (off_t)(in_piece + got));
            if (r < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return done + got ? (ssize_t)(done + got) : -1;
            }
            if (r == 0) break; /* piece shorter than when it was measured */
            got += (size_t)r;
        }
        close(fd);
        done += got;
        if (got < want) break;
    }
    return (ssize_t)done;
}
