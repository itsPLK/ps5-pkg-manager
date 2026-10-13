#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "installer.h"
#include "multipart.h"
#include "pkg_parser.h"
#include "pkg_scanner.h"
#include "split_pkg.h"
#include "test_fixture.h"

#define DIR_PATH   "/tmp/test_split_pkg"
#define SRC_PATH   "/tmp/test_split_src.pkg"
#define NAME_BASE  "EP9999-PPSA90021_00-SPLITTEST0000000"
#define PIECE_SIZE 20000u /* the fixture's inner header sits at 0x10000, i.e. in piece 3 */

static unsigned char *g_src;
static size_t g_total;

static void write_piece(unsigned idx) {
    char path[512];
    snprintf(path, sizeof(path), DIR_PATH "/" NAME_BASE "_%u.pkg", idx);
    FILE *f = fopen(path, "wb");
    assert(f);
    size_t off = (size_t)idx * PIECE_SIZE;
    size_t n = g_total - off < PIECE_SIZE ? g_total - off : PIECE_SIZE;
    assert(fwrite(g_src + off, 1, n, f) == n);
    fclose(f);
}

int main(void) {
    system("rm -rf " DIR_PATH " /tmp/pkgmgr && mkdir -p " DIR_PATH);
    assert(fixture_write_ps5_pkg(SRC_PATH, "PPSA90021", "SplitTitle", "gd", "01.000.000", 1) == 0);
    struct stat sst;
    assert(stat(SRC_PATH, &sst) == 0);
    g_total = (size_t)sst.st_size;
    assert(g_total > 3 * PIECE_SIZE && g_total < 4 * PIECE_SIZE + PIECE_SIZE); /* 4-5 pieces */
    g_src = (unsigned char *)malloc(g_total);
    assert(g_src);
    FILE *sf = fopen(SRC_PATH, "rb");
    assert(sf && fread(g_src, 1, g_total, sf) == g_total);
    fclose(sf);
    assert(g_src[0] == 0x7F);

    const unsigned pieces = (unsigned)((g_total + PIECE_SIZE - 1) / PIECE_SIZE);
    for (unsigned i = 0; i < pieces; i++) write_piece(i);
    const char *head = DIR_PATH "/" NAME_BASE "_0.pkg";
    const char *second = DIR_PATH "/" NAME_BASE "_1.pkg";

    printf("=== split_pkg detect / pread ===\n");
    split_pkg_t sp;
    assert(split_pkg_detect(head, &sp) == 1);
    assert(sp.count == pieces && sp.total == g_total);
    assert(sp.start[2] == 2 * PIECE_SIZE);
    assert(sp.size[pieces - 1] == g_total - (size_t)(pieces - 1) * PIECE_SIZE);
    assert(split_pkg_detect(second, NULL) == 0);               /* not the head */
    assert(split_pkg_detect("/tmp/nonexistent_0.pkg", NULL) == 0);
    assert(split_pkg_is_continuation(DIR_PATH, NAME_BASE "_1.pkg") == 1);
    assert(split_pkg_is_continuation(DIR_PATH, NAME_BASE "_0.pkg") == 0);
    assert(split_pkg_is_continuation(DIR_PATH, "OTHER_1.pkg") == 0); /* no piece 0 */

    unsigned char *buf = (unsigned char *)malloc(g_total);
    assert(buf);
    assert(split_pkg_pread(&sp, buf, 100, PIECE_SIZE - 50) == 100);          /* crosses 1 boundary */
    assert(memcmp(buf, g_src + PIECE_SIZE - 50, 100) == 0);
    assert(split_pkg_pread(&sp, buf, 45000, PIECE_SIZE - 50) == 45000);      /* crosses several */
    assert(memcmp(buf, g_src + PIECE_SIZE - 50, 45000) == 0);
    assert(split_pkg_pread(&sp, buf, 100, g_total - 50) == 50);              /* short at the end */
    assert(memcmp(buf, g_src + g_total - 50, 50) == 0);
    free(buf);

    printf("=== parser reads across pieces ===\n");
    pkg_detail_t d;
    assert(pkg_parser_parse(head, &d) == 0);
    printf("parsed: %s / %s size=%llu\n", d.title_id, d.title_name,
           (unsigned long long)d.total_pkg_size);
    assert(strcmp(d.title_id, "PPSA90021") == 0);
    assert(d.file_size == g_total && d.total_pkg_size == g_total);
    assert(d.is_multipart == 0);

    printf("=== scanner lists the set once ===\n");
    /* a lone _1 file without a _0 next to it is an ordinary package */
    system("cp " SRC_PATH " " DIR_PATH "/LONER_1.pkg");
    setenv("PKG_SCAN_DIR", DIR_PATH, 1);
    unsetenv("PKG_USB_PREFIX");
    unsetenv("PKG_DISC_DIR");
    unsetenv("PKG_APPMETA_DIR");
    unsetenv("PKG_ADDCONT_DIR");
    unsetenv("PKG_APP_DB_PATH");
    pkg_scanner_init();
    int count = pkg_scanner_scan();
    printf("scan count: %d (expected 2)\n", count);
    assert(count == 2);
    char *json = pkg_scanner_packages_for_drive_to_json("usb0");
    assert(strstr(json, NAME_BASE "_0.pkg") != NULL);
    assert(strstr(json, NAME_BASE "_1.pkg") == NULL);
    assert(strstr(json, NAME_BASE "_4.pkg") == NULL);
    assert(strstr(json, "LONER_1.pkg") != NULL);
    assert(strstr(json, "\"is_multipart\":false") != NULL);
    free(json);
    pkg_detail_t found;
    assert(pkg_scanner_find_by_path(head, &found) == 0);
    assert(found.file_size == g_total && found.total_pkg_size == g_total);

    int changed = -1;
    assert(pkg_scanner_scan_quick(NULL, &changed) == 2);
    printf("quick rescan changed=%d (expected 0)\n", changed);
    assert(changed == 0);

    printf("=== installer treats the set as stream-only ===\n");
    assert(installer_is_filesystem_install(head, 0) == 0);
    assert(installer_is_filesystem_install(DIR_PATH "/LONER_1.pkg", 0) == 1);

    printf("=== virtual stream joins the pieces ===\n");
    virtual_stream_t *vs = (virtual_stream_t *)calloc(1, sizeof(*vs));
    assert(vs);
    assert(virtual_stream_open(head, vs) == 0);
    assert(vs->total_pkg_size == g_total && vs->total_parts == pieces);
    assert(vs->is_multipart == 0);
    unsigned char *joined = (unsigned char *)malloc(g_total);
    assert(joined);
    uint64_t pos = 0;
    while (pos < g_total) {
        ssize_t n = virtual_stream_read(vs, pos, joined + pos, g_total - pos);
        assert(n > 0);
        pos += (uint64_t)n;
    }
    assert(memcmp(joined, g_src, g_total) == 0);
    free(joined);
    unsigned char mid[120];
    assert(virtual_stream_read(vs, PIECE_SIZE - 50, mid, 100) == 50); /* stops at the piece end */
    assert(memcmp(mid, g_src + PIECE_SIZE - 50, 50) == 0);
    virtual_stream_close(vs);
    free(vs);

    printf("=== header-declared 64 KiB tail is padded with zeros ===\n");
    {
        system("rm -rf /tmp/test_split_pad && mkdir -p /tmp/test_split_pad");
        const size_t len = 150000; /* on-disk bytes across 2 pieces */
        unsigned char *img = (unsigned char *)calloc(1, len);
        assert(img);
        memcpy(img, "\x7f" "FIH", 4);
        unsigned long long a = 0x10000ULL, b = len - 0x10000ULL, c = 0x10000ULL;
        for (int k = 0; k < 8; k++) {
            img[0x10 + k] = (unsigned char)(a >> (8 * k));
            img[0x18 + k] = (unsigned char)(b >> (8 * k));
            img[0x28 + k] = (unsigned char)(c >> (8 * k));
        }
        for (size_t i = 0x100; i < len; i++) img[i] = (unsigned char)(i * 7 + 1);
        FILE *p0 = fopen("/tmp/test_split_pad/PAD_0.pkg", "wb");
        FILE *p1 = fopen("/tmp/test_split_pad/PAD_1.pkg", "wb");
        assert(p0 && p1);
        assert(fwrite(img, 1, 100000, p0) == 100000);
        assert(fwrite(img + 100000, 1, len - 100000, p1) == len - 100000);
        fclose(p0);
        fclose(p1);
        virtual_stream_t *pv = (virtual_stream_t *)calloc(1, sizeof(*pv));
        assert(pv && virtual_stream_open("/tmp/test_split_pad/PAD_0.pkg", pv) == 0);
        assert(pv->total_pkg_size == len + 0x10000);
        unsigned char *all = (unsigned char *)malloc(len + 0x10000);
        assert(all);
        uint64_t q = 0;
        while (q < pv->total_pkg_size) {
            ssize_t n = virtual_stream_read(pv, q, all + q, pv->total_pkg_size - q);
            assert(n > 0);
            q += (uint64_t)n;
        }
        assert(memcmp(all, img, len) == 0);
        for (size_t i = len; i < len + 0x10000; i++) assert(all[i] == 0);
        assert(virtual_stream_read(pv, len + 0x10000, all, 10) == 0);
        virtual_stream_close(pv);
        free(pv);
        free(all);
        free(img);
        system("rm -rf /tmp/test_split_pad");
    }

    free(g_src);
    system("rm -rf " DIR_PATH " " SRC_PATH " /tmp/pkgmgr");
    unsetenv("PKG_SCAN_DIR");
    printf("\n>>> ALL SPLIT PKG TESTS PASSED! <<<\n");
    return 0;
}
