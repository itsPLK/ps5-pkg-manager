#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pkg_cache.h"
#include "pkg_parser.h"
#include "smb_client.h"
#include "test_fixture.h"

int main(void) {
    printf("=== Running Centralized pkg_cache Test Suite ===\n");

    /* Create temporary sandbox directory for cache tests */
    char test_dir[] = "/tmp/test_pkg_cache_XXXXXX";
    assert(mkdtemp(test_dir) != NULL);
    printf("Using test sandbox: %s\n", test_dir);

    /* Point settings and cache to sandbox */
    char settings_file[512];
    snprintf(settings_file, sizeof(settings_file), "%s/settings.json", test_dir);
    setenv("PKG_SETTINGS_PATH", settings_file, 1);

    char cache_root[512];
    snprintf(cache_root, sizeof(cache_root), "%s/cache", test_dir);
    setenv("PKG_CACHE_DIR", cache_root, 1);

    /* 1. Initialization and default settings */
    printf("[Test 1] Default settings verification\n");
    pkg_cache_init();

    app_settings_t settings;
    pkg_cache_get_settings(&settings);
    assert(settings.move_installed_to_end == 1);
    assert(settings.fade_installed_packages == 1);
    assert(settings.all_sources_mode == 0);
    assert(settings.pkg_install_debug == 0);
    assert(settings.smb_share_count == 0);
    printf("  PASSED: Default settings match specifications\n");

    /* 2. Changing settings and persistence */
    printf("[Test 2] Settings mutation & persistence\n");
    settings.move_installed_to_end = 0;
    settings.fade_installed_packages = 0;
    settings.all_sources_mode = 1;
    settings.pkg_install_debug = 1;
    settings.smb_share_count = 1;
    memset(&settings.smb_shares[0], 0, sizeof(smb_share_config_t));
    settings.smb_shares[0].enabled = 1;
    strcpy(settings.smb_shares[0].id, "smb0");
    strcpy(settings.smb_shares[0].label, "NAS Packages");
    strcpy(settings.smb_shares[0].server, "192.168.1.100");
    settings.smb_shares[0].port = 445;
    strcpy(settings.smb_shares[0].share, "pkgs");
    settings.smb_shares[0].is_read_only = 0;
    assert(pkg_cache_set_settings(&settings) == 0);

    /* Re-read settings */
    app_settings_t reloaded;
    pkg_cache_get_settings(&reloaded);
    assert(reloaded.move_installed_to_end == 0);
    assert(reloaded.fade_installed_packages == 0);
    assert(reloaded.all_sources_mode == 1);
    assert(reloaded.pkg_install_debug == 1);
    assert(reloaded.smb_share_count == 1);
    assert(strcmp(reloaded.smb_shares[0].server, "192.168.1.100") == 0);

    /* Re-init to test disk load */
    pkg_cache_init();
    pkg_cache_get_settings(&reloaded);
    assert(reloaded.move_installed_to_end == 0);
    assert(reloaded.fade_installed_packages == 0);
    assert(reloaded.all_sources_mode == 1);
    assert(reloaded.pkg_install_debug == 1);
    assert(reloaded.smb_share_count == 1);
    printf("  PASSED: Settings persisted and reloaded from disk\n");

    /* 3. Central cache directory verification */
    printf("[Test 3] Central cache directory\n");
    const char *cdir = pkg_cache_get_dir();
    assert(cdir != NULL);
    assert(strcmp(cdir, cache_root) == 0);
    printf("  PASSED: Cache dir is %s\n", cdir);

    /* 4. Checksum calculation */
    printf("[Test 4] Checksum generation\n");
    char dummy_pkg[512];
    snprintf(dummy_pkg, sizeof(dummy_pkg), "%s/sample.pkg", test_dir);
    FILE *f = fopen(dummy_pkg, "wb");
    assert(f != NULL);
    uint8_t dummy_data[8192];
    memset(dummy_data, 0xAB, sizeof(dummy_data));
    fwrite(dummy_data, 1, sizeof(dummy_data), f);
    fclose(f);

    char checksum1[64] = {0};
    assert(pkg_cache_calc_checksum(dummy_pkg, checksum1, sizeof(checksum1)) == 0);
    assert(strlen(checksum1) == 32);

    /* Recalculating without modifying file must yield same checksum */
    char checksum2[64] = {0};
    assert(pkg_cache_calc_checksum(dummy_pkg, checksum2, sizeof(checksum2)) == 0);
    assert(strcmp(checksum1, checksum2) == 0);

    /* Modify file content -> checksum must change */
    f = fopen(dummy_pkg, "r+b");
    assert(f != NULL);
    uint8_t mod = 0xCD;
    fwrite(&mod, 1, 1, f);
    fclose(f);

    char checksum3[64] = {0};
    assert(pkg_cache_calc_checksum(dummy_pkg, checksum3, sizeof(checksum3)) == 0);
    assert(strcmp(checksum1, checksum3) != 0);
    printf("  PASSED: Checksum is consistent and detects file changes\n");

    /* 5. Cache Save & Lookup (centralized, no drive_root) */
    printf("[Test 5] Central cache save and lookup\n");
    pkg_detail_t original_detail;
    memset(&original_detail, 0, sizeof(original_detail));
    strncpy(original_detail.path, dummy_pkg, sizeof(original_detail.path) - 1);
    strncpy(original_detail.filename, "sample.pkg", sizeof(original_detail.filename) - 1);
    strncpy(original_detail.title_id, "CUSA12345", sizeof(original_detail.title_id) - 1);
    strncpy(original_detail.title_name, "Test Cached Package", sizeof(original_detail.title_name) - 1);
    strncpy(original_detail.content_id, "EP0001-CUSA12345_00-TEST000000000001", sizeof(original_detail.content_id) - 1);
    strncpy(original_detail.app_version, "01.05", sizeof(original_detail.app_version) - 1);
    strncpy(original_detail.pkg_type_str, "base", sizeof(original_detail.pkg_type_str) - 1);
    original_detail.file_size = 104857600ULL;
    original_detail.pkg_type = PKG_TYPE_BASE;
    original_detail.is_valid = 1;
    original_detail.mtime = 1700000000ULL;

    assert(pkg_cache_save(checksum3, &original_detail) == 0);

    pkg_detail_t loaded_detail;
    memset(&loaded_detail, 0, sizeof(loaded_detail));
    assert(pkg_cache_lookup(checksum3, &loaded_detail) == 0);
    assert(loaded_detail.is_valid == 1);
    assert(strcmp(loaded_detail.title_id, "CUSA12345") == 0);
    assert(strcmp(loaded_detail.title_name, "Test Cached Package") == 0);
    assert(strcmp(loaded_detail.app_version, "01.05") == 0);
    assert(loaded_detail.file_size == 104857600ULL);
    assert(loaded_detail.pkg_type == PKG_TYPE_BASE);
    printf("  PASSED: Cache save and lookup accurate\n");

    /* 6. BlurHash metadata update (in-place) */
    printf("[Test 6] In-place metadata update (BlurHash)\n");
    strcpy(original_detail.blurhash, "W4IOh8,AAvmo");
    assert(pkg_cache_update_meta(checksum3, &original_detail) == 0);

    memset(&loaded_detail, 0, sizeof(loaded_detail));
    assert(pkg_cache_lookup(checksum3, &loaded_detail) == 0);
    assert(strcmp(loaded_detail.blurhash, "W4IOh8,AAvmo") == 0);
    printf("  PASSED: Metadata updated successfully\n");

    /* 7. Cache Stats and Clear */
    printf("[Test 7] Central cache stats & clearing\n");
    char *stats_json = pkg_cache_get_stats_json();
    assert(stats_json != NULL);
    printf("  Stats JSON: %s\n", stats_json);
    assert(strstr(stats_json, "\"total_count\":1") != NULL);
    assert(strstr(stats_json, cache_root) != NULL);
    free(stats_json);

    /* Clear central cache */
    int64_t freed = pkg_cache_clear();
    printf("  Freed bytes: %lld\n", (long long)freed);
    assert(freed > 0);

    /* Re-check stats: should be 0 */
    stats_json = pkg_cache_get_stats_json();
    assert(stats_json != NULL);
    assert(strstr(stats_json, "\"total_count\":0") != NULL);
    free(stats_json);

    /* Cache lookup should now fail */
    assert(pkg_cache_lookup(checksum3, &loaded_detail) == -1);
    printf("  PASSED: Central cache clearing and stats verified\n");

    /* 8. SMB share URL parsing */
    printf("[Test 8] SMB share URL parsing\n");
    char srv[128], shr[128], rel[256];
    int prt;
    assert(smb_client_parse_url("smb://192.168.1.50:4455/share/dir/file.pkg", srv, sizeof(srv), &prt, shr, sizeof(shr), rel, sizeof(rel)) == 0);
    assert(strcmp(srv, "192.168.1.50") == 0);
    assert(prt == 4455);
    assert(strcmp(shr, "share") == 0);
    assert(strcmp(rel, "dir/file.pkg") == 0);
    printf("  PASSED: SMB URL parsing verified\n");

    /* 9. SMB PKG parsing (PS4 param.sfo and PS5 param.json verification).
     * Fixtures are generated synthetically; smb_client_parse_pkg handles
     * plain local paths by falling back to the local parser. */
    printf("[Test 9] SMB PKG SFO/JSON parsing\n");
    system("rm -rf /tmp/test_cache_fixtures && mkdir -p /tmp/test_cache_fixtures");
    assert(fixture_write_ps4_pkg("/tmp/test_cache_fixtures/bq_base.pkg",
                                 "CUSA90002", "BounceQuest", "gd", "01.00") == 0);
    {
        FILE *f = fopen("/tmp/test_cache_fixtures/bq_base.pkg", "r+b");
        assert(f);
        assert(fseek(f, 4, SEEK_SET) == 0);
        const unsigned char cnt_type_one[] = {0, 0, 0, 1};
        assert(fwrite(cnt_type_one, 1, sizeof(cnt_type_one), f) == sizeof(cnt_type_one));
        fclose(f);
    }
    assert(fixture_write_ps5_pkg("/tmp/test_cache_fixtures/wc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    {
        pkg_detail_t smb_detail;
        memset(&smb_detail, 0, sizeof(smb_detail));
        int parse_res = smb_client_parse_pkg("/tmp/test_cache_fixtures/bq_base.pkg", &smb_detail);
        assert(parse_res == 0);
        assert(strcmp(smb_detail.title_id, "CUSA90002") == 0);
        assert(strcmp(smb_detail.title_name, "BounceQuest") == 0);
        assert(strcmp(smb_detail.app_version, "v01.00") == 0);
        assert(strcmp(smb_detail.category, "gd") == 0);
        assert(smb_detail.pkg_type == PKG_TYPE_BASE);

        memset(&smb_detail, 0, sizeof(smb_detail));
        parse_res = smb_client_parse_pkg("/tmp/test_cache_fixtures/wc.pkg", &smb_detail);
        assert(parse_res == 0);
        assert(strcmp(smb_detail.title_id, "PPSA90012") == 0);
        assert(strcmp(smb_detail.title_name, "WaveCast") == 0);
        assert(smb_detail.pkg_type == PKG_TYPE_BASE);
        assert(strcmp(smb_detail.pkg_type_str, "base") == 0);
        printf("  PASSED: SMB PKG parsing successfully extracts Title, TitleID, and Version from SFO & JSON\n");
    }

    /* 10. Stale / corrupted cache rejection */
    printf("[Test 10] Stale 'Unknown Package' cache invalidation\n");
    pkg_detail_t bad_detail;
    memset(&bad_detail, 0, sizeof(bad_detail));
    strcpy(bad_detail.path, "/tmp/bad_pkg.pkg");
    strcpy(bad_detail.title_id, "");
    strcpy(bad_detail.title_name, "Unknown Package");
    bad_detail.is_valid = 1;
    char bad_checksum[] = "bad_checksum_12345";
    assert(pkg_cache_save(bad_checksum, &bad_detail) == 0);

    /* Lookup should reject the corrupted entry and return -1 */
    pkg_detail_t check_detail;
    assert(pkg_cache_lookup(bad_checksum, &check_detail) == -1);
    printf("  PASSED: Stale Unknown Package cache entry successfully rejected\n");

    /* 11. SMB connection test validation and share parsing */
    printf("[Test 11] SMB connection testing validation and parsing\n");
    char err_buf[512] = {0};
    assert(smb_client_test_connection(NULL, err_buf, sizeof(err_buf)) == -1);
    assert(strcmp(err_buf, "No configuration provided") == 0);

    smb_share_config_t bad_cfg;
    memset(&bad_cfg, 0, sizeof(bad_cfg));
    assert(smb_client_test_connection(&bad_cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "Invalid share configuration") != NULL);

    /* Test with only server, missing share */
    strcpy(bad_cfg.server, "192.168.1.1");
    assert(smb_client_test_connection(&bad_cfg, err_buf, sizeof(err_buf)) == -1);
    assert(strstr(err_buf, "Invalid share configuration") != NULL);

    /* Test smb_client_sanitize_config */
    smb_share_config_t san_cfg;
    memset(&san_cfg, 0, sizeof(san_cfg));
    strcpy(san_cfg.server, " smb://192.168.1.100:1445/myshare/subfolder/ ");
    strcpy(san_cfg.path, " \\extra\\path\\ ");
    san_cfg.port = 0;
    smb_client_sanitize_config(&san_cfg);
    assert(strcmp(san_cfg.server, "192.168.1.100") == 0);
    assert(san_cfg.port == 1445);
    assert(strcmp(san_cfg.share, "myshare") == 0);
    assert(strcmp(san_cfg.path, "subfolder/extra/path") == 0);
    assert(strcmp(san_cfg.workgroup, "WORKGROUP") == 0);

    /* Test IPv6 addresses: raw IPv6 should not corrupt port, bracketed IPv6 with port */
    memset(&san_cfg, 0, sizeof(san_cfg));
    strcpy(san_cfg.server, " 2001:db8::1 ");
    strcpy(san_cfg.share, " games ");
    smb_client_sanitize_config(&san_cfg);
    assert(strcmp(san_cfg.server, "2001:db8::1") == 0);
    assert(san_cfg.port == 445);

    memset(&san_cfg, 0, sizeof(san_cfg));
    strcpy(san_cfg.server, " [2001:db8::1]:1445 ");
    strcpy(san_cfg.share, " games ");
    smb_client_sanitize_config(&san_cfg);
    assert(strcmp(san_cfg.server, "[2001:db8::1]") == 0);
    assert(san_cfg.port == 1445);

    /* Test server containing subpath when share was already set */
    memset(&san_cfg, 0, sizeof(san_cfg));
    strcpy(san_cfg.server, " 192.168.1.100/myshare/subfolder ");
    strcpy(san_cfg.share, " myshare ");
    smb_client_sanitize_config(&san_cfg);
    assert(strcmp(san_cfg.server, "192.168.1.100") == 0);
    assert(strcmp(san_cfg.share, "myshare") == 0);
    assert(strcmp(san_cfg.path, "subfolder") == 0);

    /* Test backslash server and trailing slash share */
    memset(&san_cfg, 0, sizeof(san_cfg));
    strcpy(san_cfg.server, " \\\\10.0.0.5\\ ");
    strcpy(san_cfg.share, " /my_pkgs/ ");
    strcpy(san_cfg.path, " \\ps5\\updates\\ ");
    smb_client_sanitize_config(&san_cfg);
    assert(strcmp(san_cfg.server, "10.0.0.5") == 0);
    assert(strcmp(san_cfg.share, "my_pkgs") == 0);
    assert(strcmp(san_cfg.path, "ps5/updates") == 0);
    assert(san_cfg.port == 445);

    /* Test mock SMB connection test success paths */
    system("rm -rf /tmp/mock_smb && mkdir -p /tmp/mock_smb");
    smb_share_config_t mock_cfg;
    memset(&mock_cfg, 0, sizeof(mock_cfg));
    strcpy(mock_cfg.server, "smb://192.168.1.50");
    strcpy(mock_cfg.share, "packages");
    mock_cfg.is_read_only = 0;

    int test_res = smb_client_test_connection(&mock_cfg, err_buf, sizeof(err_buf));
    assert(test_res == 0);
    assert(strstr(err_buf, "Connected successfully (Read/Write)") != NULL);

    /* Test read-only flag */
    mock_cfg.is_read_only = 1;
    test_res = smb_client_test_connection(&mock_cfg, err_buf, sizeof(err_buf));
    assert(test_res == 0);
    assert(strstr(err_buf, "Connected successfully (Read-Only)") != NULL);
    system("rm -rf /tmp/mock_smb");

    /* Test parsing smb_shares */
    smb_share_config_t parsed_shares[2];
    int parsed_count = 0;
    const char *test_json = "{\"smb_shares\":[{\"server\":\"//192.168.1.100/\",\"share\":\"/games/\",\"path\":\"\\\\sub\\\\dir\\\\\",\"username\":\"\",\"password\":\"secret\",\"port\":445}]}";
    pkg_cache_parse_smb_shares(test_json, parsed_shares, &parsed_count);
    assert(parsed_count == 1);
    assert(strcmp(parsed_shares[0].server, "//192.168.1.100/") == 0);
    assert(strcmp(parsed_shares[0].share, "/games/") == 0);
    assert(strcmp(parsed_shares[0].password, "secret") == 0);

    /* Sanitize parsed share */
    smb_client_sanitize_config(&parsed_shares[0]);
    assert(strcmp(parsed_shares[0].server, "192.168.1.100") == 0);
    assert(strcmp(parsed_shares[0].share, "games") == 0);
    assert(strcmp(parsed_shares[0].path, "sub/dir") == 0);

    /* Test parsing smb_shares with embedded share in server and blank share field */
    parsed_count = 0;
    const char *test_json_embedded = "{\"smb_shares\":[{\"server\":\"smb://192.168.1.200/media\",\"share\":\"\",\"username\":\"guest\"}]}";
    pkg_cache_parse_smb_shares(test_json_embedded, parsed_shares, &parsed_count);
    assert(parsed_count == 1);
    smb_client_sanitize_config(&parsed_shares[0]);
    assert(strcmp(parsed_shares[0].server, "192.168.1.200") == 0);
    assert(strcmp(parsed_shares[0].share, "media") == 0);
    printf("  PASSED: SMB connection test validation and share parsing verified\n");

    /* Cleanup */
    unlink(dummy_pkg);
    unlink(settings_file);
    pkg_cache_clear();
    rmdir(cache_root);
    rmdir(test_dir);

    printf("\n=== All pkg_cache tests PASSED! ===\n");
    return 0;
}
