#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "pkg_parser.h"
#include "multipart.h"
#include "test_fixture.h"

int main(int argc, char **argv) {
    /* Default fixtures are synthetic; a real package can be passed as an
     * optional regression check. */
    system("rm -rf /tmp/test_parser_fixtures && mkdir -p /tmp/test_parser_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_parser_fixtures/nv.pkg",
                                 "PPSA90011", "NebulaView", "gd", "06.000.000", 1) == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_parser_fixtures/wc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_parser_fixtures/bq_base.pkg",
                                 "CUSA90002", "BounceQuest", "gd", "01.00") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_parser_fixtures/bq_upd.pkg",
                                 "CUSA90002", "BounceQuest", "gp", "01.06") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_parser_fixtures/ab_bonus.pkg",
                                 "CUSA90003", "Aerobeat: Bonus Tracks", "ac", "01.00") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_parser_fixtures/ab_sunset.pkg",
                                 "CUSA90003", "Aerobeat: Sunset Pack", "ac", "01.00") == 0);

    /* 1. PS5 Base */
    const char *pkg1 = "/tmp/test_parser_fixtures/nv.pkg";
    printf("=== Testing pkg1: %s ===\n", pkg1);
    pkg_detail_t detail1;
    int res1 = pkg_parser_parse(pkg1, &detail1);
    assert(res1 == 0);
    assert(detail1.is_valid == 1);
    assert(strcmp(detail1.title_id, "PPSA90011") == 0);
    assert(strcmp(detail1.title_name, "NebulaView") == 0);
    assert(strcmp(detail1.app_version, "v6.00") == 0);
    assert(detail1.has_icon == 1);
    assert(detail1.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(detail1.pkg_type_str, "base") == 0);
    assert(detail1.mtime > 0);

    /* 2. PS5 Base */
    const char *pkg2 = "/tmp/test_parser_fixtures/wc.pkg";
    printf("=== Testing pkg2: %s ===\n", pkg2);
    pkg_detail_t detail2;
    int res2 = pkg_parser_parse(pkg2, &detail2);
    assert(res2 == 0);
    assert(detail2.is_valid == 1);
    assert(strcmp(detail2.title_id, "PPSA90012") == 0);
    assert(strcmp(detail2.title_name, "WaveCast") == 0);
    assert(strcmp(detail2.app_version, "v1.03") == 0);
    assert(detail2.has_icon == 1);
    assert(detail2.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(detail2.pkg_type_str, "base") == 0);

    /* PS5 DLC metadata variant: the CNT package-type word may be 0x00020001, not
     * exactly 1, and param.json may still carry a base-like category. */
    const char *pkg2_dlc = "/tmp/test_parser_fixtures/wc_dlc_variant.pkg";
    assert(fixture_write_ps5_pkg(pkg2_dlc, "TEST00012", "Synthetic DLC Package", "gd",
                                 "01.000.000", 0) == 0);
    FILE *dlc_file = fopen(pkg2_dlc, "r+b");
    assert(dlc_file != NULL);
    assert(fseek(dlc_file, 0x10004, SEEK_SET) == 0); /* CNT offset + 0x04 */
    const unsigned char dlc_type[] = {0x00, 0x02, 0x00, 0x01};
    assert(fwrite(dlc_type, 1, sizeof(dlc_type), dlc_file) == sizeof(dlc_type));
    fclose(dlc_file);

    pkg_detail_t detail2_dlc;
    assert(pkg_parser_parse(pkg2_dlc, &detail2_dlc) == 0);
    assert(detail2_dlc.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(detail2_dlc.pkg_type_str, "dlc") == 0);

    /* Test icon extraction on pkg2 */
    uint8_t *icon_data = NULL;
    size_t icon_size = 0;
    int res_icon = pkg_parser_get_icon(pkg2, detail2.icon_offset, detail2.icon_size, &icon_data, &icon_size);
    assert(res_icon == 0);
    assert(icon_size == detail2.icon_size);
    assert(memcmp(icon_data, "\x89PNG\r\n\x1a\n", 8) == 0);
    printf("Icon verified! Magic: PNG, size: %zu bytes\n", icon_size);
    free(icon_data);

    /* 3. PS4 Base Package */
    const char *pkg3 = "/tmp/test_parser_fixtures/bq_base.pkg";
    printf("=== Testing pkg3: %s ===\n", pkg3);
    pkg_detail_t detail3;
    int res3 = pkg_parser_parse(pkg3, &detail3);
    assert(res3 == 0);
    assert(detail3.is_valid == 1);
    assert(strcmp(detail3.title_id, "CUSA90002") == 0);
    assert(strcmp(detail3.title_name, "BounceQuest") == 0);
    assert(strcmp(detail3.app_version, "v01.00") == 0);
    assert(detail3.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(detail3.pkg_type_str, "base") == 0);
    assert(strcmp(detail3.category, "gd") == 0);

    /* PS4 base packages may use CNT type 1 even with CATEGORY=gd. */
    FILE *base_file = fopen(pkg3, "r+b");
    assert(base_file != NULL);
    assert(fseek(base_file, 4, SEEK_SET) == 0);
    const uint8_t cnt_type_one[4] = {0, 0, 0, 1};
    assert(fwrite(cnt_type_one, 1, sizeof(cnt_type_one), base_file) == sizeof(cnt_type_one));
    fclose(base_file);
    assert(pkg_parser_parse(pkg3, &detail3) == 0);
    assert(strcmp(detail3.category, "gd") == 0);
    assert(detail3.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(detail3.pkg_type_str, "base") == 0);

    /* 4. PS4 Update (v01.06) */
    const char *pkg4 = "/tmp/test_parser_fixtures/bq_upd.pkg";
    printf("=== Testing pkg4: %s ===\n", pkg4);
    pkg_detail_t detail4;
    int res4 = pkg_parser_parse(pkg4, &detail4);
    assert(res4 == 0);
    assert(detail4.is_valid == 1);
    assert(strcmp(detail4.title_id, "CUSA90002") == 0);
    assert(strcmp(detail4.title_name, "BounceQuest") == 0);
    assert(strcmp(detail4.app_version, "v01.06") == 0);
    assert(detail4.pkg_type == PKG_TYPE_UPDATE);
    assert(strcmp(detail4.pkg_type_str, "update") == 0);
    assert(strcmp(detail4.category, "gp") == 0);

    /* 5. PS4 DLC (music pack) */
    const char *pkg5 = "/tmp/test_parser_fixtures/ab_bonus.pkg";
    printf("=== Testing pkg5: %s ===\n", pkg5);
    pkg_detail_t detail5;
    int res5 = pkg_parser_parse(pkg5, &detail5);
    assert(res5 == 0);
    assert(detail5.is_valid == 1);
    assert(strcmp(detail5.title_id, "CUSA90003") == 0);
    assert(strcmp(detail5.title_name, "Aerobeat: Bonus Tracks") == 0);
    assert(detail5.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(detail5.pkg_type_str, "dlc") == 0);
    assert(strcmp(detail5.category, "ac") == 0);

    /* 6. PS4 DLC (bonus tracks) */
    const char *pkg6 = "/tmp/test_parser_fixtures/ab_sunset.pkg";
    printf("=== Testing pkg6: %s ===\n", pkg6);
    pkg_detail_t detail6;
    int res6 = pkg_parser_parse(pkg6, &detail6);
    assert(res6 == 0);
    assert(detail6.is_valid == 1);
    assert(strcmp(detail6.title_id, "CUSA90003") == 0);
    assert(strcmp(detail6.title_name, "Aerobeat: Sunset Pack") == 0);
    assert(detail6.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(detail6.pkg_type_str, "dlc") == 0);
    assert(strcmp(detail6.category, "ac") == 0);

    /* 7. Multi-Part PKG Type Handling */
    printf("=== Testing Multi-Part Header Types (update & dlc) ===\n");
    system("mkdir -p /tmp/test_mpart_types");
    multipart_header_t mhdr_up;
    memset(&mhdr_up, 0, sizeof(mhdr_up));
    memcpy(mhdr_up.magic, MULTIPART_MAGIC, MULTIPART_MAGIC_LEN);
    mhdr_up.header_version = 1;
    mhdr_up.part_index = 1;
    mhdr_up.total_parts = 2;
    mhdr_up.chunk_size = 1024 * 1024;
    mhdr_up.compression_type = MULTIPART_COMPRESSION_NONE;
    strncpy(mhdr_up.title_id, "PPSA90012", sizeof(mhdr_up.title_id) - 1);
    strncpy(mhdr_up.title_name, "WaveCast Update", sizeof(mhdr_up.title_name) - 1);
    strncpy(mhdr_up.pkg_type, "update", sizeof(mhdr_up.pkg_type) - 1);
    FILE *fup = fopen("/tmp/test_mpart_types/test.part1", "wb");
    assert(fup != NULL);
    fwrite(&mhdr_up, 1, sizeof(mhdr_up), fup);
    fclose(fup);

    pkg_detail_t mdetail;
    int mres = pkg_parser_parse("/tmp/test_mpart_types/test.part1", &mdetail);
    assert(mres == 0);
    assert(mdetail.is_multipart == 1);
    assert(mdetail.pkg_type == PKG_TYPE_UPDATE);
    assert(strcmp(mdetail.pkg_type_str, "update") == 0);

    /* Test DLC multipart */
    strncpy(mhdr_up.pkg_type, "dlc", sizeof(mhdr_up.pkg_type) - 1);
    fup = fopen("/tmp/test_mpart_types/test.part1", "wb");
    assert(fup != NULL);
    fwrite(&mhdr_up, 1, sizeof(mhdr_up), fup);
    fclose(fup);

    mres = pkg_parser_parse("/tmp/test_mpart_types/test.part1", &mdetail);
    assert(mres == 0);
    assert(mdetail.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(mdetail.pkg_type_str, "dlc") == 0);

    system("rm -rf /tmp/test_mpart_types");

    /* 8. Multi-Language Title Resolution */
    printf("=== Testing Multi-Language Title Resolution ===\n");
    assert(fixture_write_ps5_pkg_multilang("/tmp/test_parser_fixtures/multilang.pkg",
                                           "PPSA90099", "en-US", "gd", "01.000.000", 0) == 0);
    pkg_detail_t mldetail;
    int mlres = pkg_parser_parse("/tmp/test_parser_fixtures/multilang.pkg", &mldetail);
    assert(mlres == 0);
    assert(mldetail.is_valid == 1);
    /* Should NOT pick Arabic Title despite ar-AE appearing first in JSON */
    assert(strcmp(mldetail.title_name, "English Title") == 0);
    assert(strcmp(mldetail.default_language, "en-US") == 0);
    assert(strstr(mldetail.localized_titles, "\"ar-AE\":\"Arabic Title\"") != NULL);
    assert(strstr(mldetail.localized_titles, "\"en-US\":\"English Title\"") != NULL);
    assert(strstr(mldetail.localized_titles, "\"pl-PL\":\"Polish Title\"") != NULL);

    /* Test pkg_parser_resolve_localized_title */
    char resolved[PKG_TITLE_NAME_LEN];
    assert(pkg_parser_resolve_localized_title(mldetail.localized_titles, mldetail.default_language,
                                              "ar-AE", resolved, sizeof(resolved)) == 0);
    assert(strcmp(resolved, "Arabic Title") == 0);

    assert(pkg_parser_resolve_localized_title(mldetail.localized_titles, mldetail.default_language,
                                              "ar,en;q=0.9", resolved, sizeof(resolved)) == 0);
    assert(strcmp(resolved, "Arabic Title") == 0);

    assert(pkg_parser_resolve_localized_title(mldetail.localized_titles, mldetail.default_language,
                                              "pl", resolved, sizeof(resolved)) == 0);
    assert(strcmp(resolved, "Polish Title") == 0);

    /* Fallback to default_language (en-US) when requested language not found */
    assert(pkg_parser_resolve_localized_title(mldetail.localized_titles, mldetail.default_language,
                                              "fr-FR,fr;q=0.9", resolved, sizeof(resolved)) == 0);
    assert(strcmp(resolved, "English Title") == 0);

    /* Optional real-world regression fixture, supplied by the caller. */
    if (argc > 1) {
        pkg_detail_t reference;
        assert(pkg_parser_parse(argv[1], &reference) == 0);
        assert(strcmp(reference.category, "gd") == 0);
        assert(reference.pkg_type == PKG_TYPE_BASE);
        assert(strcmp(reference.pkg_type_str, "base") == 0);
        printf("Reference package classified as base: %s\n", argv[1]);
    }

    printf("\n>>> ALL PKG PARSER TESTS (6 PACKAGES + MULTIPART TYPES + MULTILANG) PASSED! <<<\n");
    return 0;
}
