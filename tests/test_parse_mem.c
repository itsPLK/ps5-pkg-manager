/*
 * Host test: pkg_parser_parse_mem matches pkg_parser_parse (NEW additive
 * function; the file parser itself is untouched). Guards against logic
 * drift between the two paths that installer_start[_live] depend on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "pkg_parser.h"
#include "multipart.h"
#include "test_fixture.h"

#define PM_DIR "/tmp/test_parse_mem"

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long sz = ftell(f);
    assert(sz > 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    uint8_t *b = malloc((size_t)sz);
    assert(b);
    assert(fread(b, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    *out_len = (size_t)sz;
    return b;
}

static void expect_equal(const pkg_detail_t *a, const pkg_detail_t *b,
                         const char *what) {
    assert(strcmp(a->title_id, b->title_id) == 0);
    assert(strcmp(a->title_name, b->title_name) == 0);
    assert(strcmp(a->content_id, b->content_id) == 0);
    assert(strcmp(a->app_version, b->app_version) == 0);
    assert(strcmp(a->category, b->category) == 0);
    assert(a->pkg_type == b->pkg_type);
    assert(strcmp(a->pkg_type_str, b->pkg_type_str) == 0);
    assert(a->is_multipart == b->is_multipart);
    printf("  match %s: tid='%s' name='%.32s' ver='%s' kind='%s'\n", what,
           a->title_id, a->title_name, a->app_version, a->pkg_type_str);
}

static void test_ps5_mem(void) {
    const char *p = PM_DIR "/ps5.pkg";
    assert(fixture_write_ps5_pkg(p, "PPSA90012", "MemGame", "gd",
                                 "01.000.000", 1) == 0);
    pkg_detail_t file_d;
    assert(pkg_parser_parse(p, &file_d) == 0);
    size_t len = 0;
    uint8_t *buf = read_file(p, &len);
    pkg_detail_t mem_d;
    assert(pkg_parser_parse_mem(buf, len, len, "MemGame.pkg", &mem_d, NULL) == 0);
    expect_equal(&file_d, &mem_d, "ps5");
    assert(mem_d.has_icon == 0); /* live skips icon extraction */
    free(buf);
}

static void test_ps4_mem(void) {
    const char *p = PM_DIR "/ps4.pkg";
    assert(fixture_write_ps4_pkg(p, "CUSA90013", "MemFour", "gd", "01.00") == 0);
    pkg_detail_t file_d;
    assert(pkg_parser_parse(p, &file_d) == 0);
    size_t len = 0;
    uint8_t *buf = read_file(p, &len);
    pkg_detail_t mem_d;
    assert(pkg_parser_parse_mem(buf, len, len, "MemFour.pkg", &mem_d, NULL) == 0);
    expect_equal(&file_d, &mem_d, "ps4");
    free(buf);
}

static void test_truncated_fails(void) {
    const char *p = PM_DIR "/ps5.pkg";
    size_t len = 0;
    uint8_t *buf = read_file(p, &len);
    assert(len > 128);
    pkg_detail_t mem_d;
    /* 64 bytes: no CNT discoverable -> must fail closed, not misparse. */
    int stage = 0;
    assert(pkg_parser_parse_mem(buf, 64, len, "x.pkg", &mem_d, &stage) != 0);
    assert(stage == 1 || stage == 2);
    free(buf);
    printf("  truncated-fails ok\n");
}

static void test_multipart_mem(void) {
    multipart_header_t h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN);
    h.header_version = 1;
    h.part_index = 1;
    h.total_parts = 2;
    snprintf(h.pkg_filename, sizeof(h.pkg_filename), "Big.pkg");
    snprintf(h.title_id, sizeof(h.title_id), "PPSA90014");
    snprintf(h.title_name, sizeof(h.title_name), "BigGame");
    snprintf(h.content_id, sizeof(h.content_id), "EP9000-PPSA90014_00-BIGGAME000001");
    snprintf(h.app_version, sizeof(h.app_version), "01.000.000");
    snprintf(h.pkg_type, sizeof(h.pkg_type), "base");
    h.total_pkg_size = 50ULL * 1024 * 1024 * 1024;
    pkg_detail_t mem_d;
    assert(pkg_parser_parse_mem((const uint8_t *)&h, sizeof(h),
                                h.total_pkg_size, "live:abc", &mem_d, NULL) == 0);
    assert(mem_d.is_multipart == 1 && mem_d.total_parts == 2);
    assert(strcmp(mem_d.title_id, "PPSA90014") == 0);
    assert(strcmp(mem_d.pkg_type_str, "base") == 0);
    assert(strcmp(mem_d.filename, "live:abc.pkg") == 0);
    printf("  multipart-mem ok\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("==============================================\n");
    printf(">>> RUNNING PARSE_MEM DIFFERENTIAL TEST <<<\n");
    printf("==============================================\n");
    assert(system("rm -rf " PM_DIR " && mkdir -p " PM_DIR) == 0);
    test_ps5_mem();
    test_ps4_mem();
    test_truncated_fails();
    test_multipart_mem();
    printf("\n>>> ALL PARSE_MEM TESTS PASSED! <<<\n");
    return 0;
}
