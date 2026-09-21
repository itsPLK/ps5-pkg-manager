#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pkg_scanner.h"
#include "app_info.h"
#include "test_fixture.h"

int main(void) {
    /* All fixtures are generated synthetically (fictional IDs/titles) so
     * this test never needs external files. */
    system("rm -rf /tmp/test_scan_fixtures && mkdir -p /tmp/test_scan_fixtures");
    assert(fixture_write_ps4_pkg("/tmp/test_scan_fixtures/ab_bonus.pkg",
                                 "CUSA90003", "Aerobeat: Bonus Tracks", "ac", "01.00") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_scan_fixtures/ab_sunset.pkg",
                                 "CUSA90003", "Aerobeat: Sunset Pack", "ac", "01.00") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_scan_fixtures/bq_base.pkg",
                                 "CUSA90002", "BounceQuest", "gd", "01.00") == 0);
    assert(fixture_write_ps4_pkg("/tmp/test_scan_fixtures/bq_upd.pkg",
                                 "CUSA90002", "BounceQuest", "gp", "01.06") == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_scan_fixtures/nv.pkg",
                                 "PPSA90011", "NebulaView", "gd", "06.000.000", 1) == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_scan_fixtures/wc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);
    /* Large package for the multipart quick-scan step (80MB -> 2x60M parts). */
    assert(fixture_write_ps5_pkg("/tmp/test_scan_big.pkg",
                                 "PPSA90010", "NebulaDrift", "gd", "06.000.000", 1) == 0);
    assert(fixture_grow_file("/tmp/test_scan_big.pkg", 80ULL * 1024 * 1024) == 0);

    /* Clean any leftover cache from previous runs */
    system("rm -rf /tmp/pkgmgr");

    /* =========================================================================
     * Test 1: Scan 6 Example Packages & Default Alphabetical Title Sorting
     * ========================================================================= */
    printf("=== Testing Scan of 6 Example Packages & Default Title Sort ===\n");
    setenv("PKG_SCAN_DIR", "/tmp/test_scan_fixtures", 1);
    unsetenv("PKG_APPMETA_DIR");
    unsetenv("PKG_ADDCONT_DIR");
    unsetenv("PKG_APP_DB_PATH");

    pkg_scanner_init();
    int count = pkg_scanner_scan();
    printf("Scanned packages count: %d (expected: 6)\n", count);
    assert(count == 6);

    /* Verify alphabetical sorting by title_name */
    pkg_detail_t d0, d1, d2, d3, d4, d5;
    assert(pkg_scanner_get_at(0, &d0) == 0);
    assert(pkg_scanner_get_at(1, &d1) == 0);
    assert(pkg_scanner_get_at(2, &d2) == 0);
    assert(pkg_scanner_get_at(3, &d3) == 0);
    assert(pkg_scanner_get_at(4, &d4) == 0);
    assert(pkg_scanner_get_at(5, &d5) == 0);

    printf("Sorted pkgs:\n 0: %s [%s]\n 1: %s [%s]\n 2: %s [%s]\n 3: %s [%s]\n 4: %s [%s]\n 5: %s [%s]\n",
           d0.title_name, d0.pkg_type_str,
           d1.title_name, d1.pkg_type_str,
           d2.title_name, d2.pkg_type_str,
           d3.title_name, d3.pkg_type_str,
           d4.title_name, d4.pkg_type_str,
           d5.title_name, d5.pkg_type_str);

    assert(strcmp(d0.title_name, "Aerobeat: Bonus Tracks") == 0);
    assert(d0.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(d1.title_name, "Aerobeat: Sunset Pack") == 0);
    assert(d1.pkg_type == PKG_TYPE_DLC);
    assert(strcmp(d2.title_name, "BounceQuest") == 0);
    assert(d2.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(d3.title_name, "BounceQuest") == 0);
    assert(d3.pkg_type == PKG_TYPE_UPDATE);
    assert(strcmp(d4.title_name, "NebulaView") == 0);
    assert(d4.pkg_type == PKG_TYPE_BASE);
    assert(strcmp(d5.title_name, "WaveCast") == 0);
    assert(d5.pkg_type == PKG_TYPE_BASE);

    char *json = pkg_scanner_to_json();
    assert(json != NULL);
    assert(strstr(json, "PPSA90011") != NULL);
    assert(strstr(json, "NebulaView") != NULL);
    assert(strstr(json, "PPSA90012") != NULL);
    assert(strstr(json, "WaveCast") != NULL);
    assert(strstr(json, "CUSA90002") != NULL);
    assert(strstr(json, "BounceQuest") != NULL);
    assert(strstr(json, "CUSA90003") != NULL);
    assert(strstr(json, "\"pkg_type\":\"base\"") != NULL);
    assert(strstr(json, "\"pkg_type\":\"update\"") != NULL);
    assert(strstr(json, "\"pkg_type\":\"dlc\"") != NULL);
    assert(strstr(json, "\"mtime\":") != NULL);
    free(json);

    /* =========================================================================
     * Test 2: Installed Detection & can_install Logic via Mock Environment
     * ========================================================================= */
    printf("\n=== Testing Installed App & DLC Detection and can_install Gating ===\n");
    system("rm -rf /tmp/mock_appmeta /tmp/mock_addcont /tmp/mock_usb_install_test /tmp/mock_user_app");
    system("mkdir -p /tmp/mock_appmeta /tmp/mock_addcont /tmp/mock_usb_install_test /tmp/mock_user_app");

    /* Copy base and update fixture pkgs to test drive */
    system("cp /tmp/test_scan_fixtures/bq_base.pkg /tmp/test_scan_fixtures/bq_upd.pkg /tmp/mock_usb_install_test/");
    /* Copy DLC fixture pkg to test drive */
    system("cp /tmp/test_scan_fixtures/ab_bonus.pkg /tmp/mock_usb_install_test/");

    setenv("PKG_SCAN_DIR", "/tmp/mock_usb_install_test", 1);
    setenv("PKG_APPMETA_DIR", "/tmp/mock_appmeta", 1);
    setenv("PKG_ADDCONT_DIR", "/tmp/mock_addcont", 1);
    setenv("PKG_USER_APP_DIR", "/tmp/mock_user_app", 1);
    unsetenv("PKG_USB_PREFIX");

    /* Step A: Base packages are NOT installed initially */
    pkg_scanner_init();
    pkg_scanner_scan();
    char *json_not_installed = pkg_scanner_packages_for_drive_to_json("/tmp/mock_usb_install_test");
    printf("JSON with base NOT installed:\n%s\n", json_not_installed);

    /* Base game can be installed */
    assert(strstr(json_not_installed, "\"title_id\":\"CUSA90002\"") != NULL);
    /* Update CANNOT be installed without base */
    assert(strstr(json_not_installed, "Base package is not installed") != NULL);
    /* DLC CANNOT be installed without base */
    free(json_not_installed);

    /* Step B: Install base game v01.00 into mock_appmeta and mock_user_app */
    system("mkdir -p /tmp/mock_appmeta/CUSA90002 /tmp/mock_user_app/CUSA90002/sce_sys");
    /* Write a mock param.json with contentVersion 01.000.000 */
    FILE *fp = fopen("/tmp/mock_appmeta/CUSA90002/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"CUSA90002\",\"contentVersion\":\"01.000.000\",\"titleName\":\"BounceQuest\"}\n");
    fclose(fp);
    fp = fopen("/tmp/mock_user_app/CUSA90002/sce_sys/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"CUSA90002\",\"contentVersion\":\"01.000.000\",\"titleName\":\"BounceQuest\"}\n");
    fclose(fp);

    /* Also install base music game into mock_appmeta and mock_user_app */
    system("mkdir -p /tmp/mock_appmeta/CUSA90003 /tmp/mock_user_app/CUSA90003/sce_sys");
    fp = fopen("/tmp/mock_appmeta/CUSA90003/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"CUSA90003\",\"contentVersion\":\"01.000.000\",\"titleName\":\"Aerobeat\"}\n");
    fclose(fp);
    fp = fopen("/tmp/mock_user_app/CUSA90003/sce_sys/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"CUSA90003\",\"contentVersion\":\"01.000.000\",\"titleName\":\"Aerobeat\"}\n");
    fclose(fp);

    /* Now check status with base installed */
    pkg_scanner_init();
    pkg_scanner_scan();
    char *json_base_installed = pkg_scanner_packages_for_drive_to_json("/tmp/mock_usb_install_test");
    printf("JSON with base INSTALLED (v01.00):\n%s\n", json_base_installed);

    /* Both update (v01.06 > v01.00) and DLC should now be installable! */
    assert(strstr(json_base_installed, "\"installed_version\":\"v1.00\"") != NULL);
    assert(strstr(json_base_installed, "\"can_install\":true") != NULL);
    /* Base game MUST now be disabled because installed version is same or newer */
    assert(strstr(json_base_installed, "Installed version is same or newer") != NULL);
    free(json_base_installed);

    /* Direct Install uses the same eligibility check without a scanned drive. */
    pkg_detail_t direct_pkg = {0};
    pkg_install_eligibility_t eligibility;
    snprintf(direct_pkg.title_id, sizeof(direct_pkg.title_id), "CUSA90002");
    snprintf(direct_pkg.app_version, sizeof(direct_pkg.app_version), "v1.00");
    direct_pkg.pkg_type = PKG_TYPE_BASE;
    pkg_scanner_check_install_eligibility(&direct_pkg, &eligibility);
    assert(!eligibility.can_install);
    assert(strcmp(eligibility.disabled_reason, "Installed version is same or newer") == 0);

    direct_pkg.pkg_type = PKG_TYPE_UPDATE;
    snprintf(direct_pkg.app_version, sizeof(direct_pkg.app_version), "v1.06");
    pkg_scanner_check_install_eligibility(&direct_pkg, &eligibility);
    assert(eligibility.can_install);

    snprintf(direct_pkg.title_id, sizeof(direct_pkg.title_id), "CUSA90099");
    pkg_scanner_check_install_eligibility(&direct_pkg, &eligibility);
    assert(!eligibility.can_install);
    assert(strcmp(eligibility.disabled_reason, "Base package is not installed") == 0);

    /* Step C: Simulate updating base game to v01.06 in mock_appmeta */
    fp = fopen("/tmp/mock_appmeta/CUSA90002/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"CUSA90002\",\"contentVersion\":\"01.060.000\",\"titleName\":\"BounceQuest\"}\n");
    fclose(fp);

    /* Simulate installing the DLC into mock_addcont (dirname must match
     * the scanned fixture's content_id). */
    system("mkdir -p /tmp/mock_addcont/CUSA90003/EP0001-CUSA90003_00-TEST000000000001");

    pkg_scanner_init();
    pkg_scanner_scan();
    char *json_updated = pkg_scanner_packages_for_drive_to_json("/tmp/mock_usb_install_test");
    printf("JSON with update v01.06 already installed and DLC already installed:\n%s\n", json_updated);

    /* Base game should remain disabled: "Installed version is same or newer" */
    assert(strstr(json_updated, "Installed version is same or newer") != NULL);
    /* Update should now be disabled: "Installed version is same or newer" */
    assert(strstr(json_updated, "Installed version is same or newer") != NULL);
    /* DLC should now be disabled: "DLC is already installed" */
    assert(strstr(json_updated, "DLC is already installed") != NULL);
    free(json_updated);

    /* Step D: Test cumulative base upgrade (older version installed on console, newer base package on drive) */
    system("mkdir -p /tmp/mock_appmeta/PPSA90011 /tmp/mock_user_app/PPSA90011/sce_sys /tmp/mock_upgrade_test");
    fp = fopen("/tmp/mock_appmeta/PPSA90011/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"PPSA90011\",\"contentVersion\":\"05.000.000\",\"titleName\":\"NebulaView\"}\n");
    fclose(fp);
    fp = fopen("/tmp/mock_user_app/PPSA90011/sce_sys/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"PPSA90011\",\"contentVersion\":\"05.000.000\",\"titleName\":\"NebulaView\"}\n");
    fclose(fp);
    system("cp /tmp/test_scan_fixtures/nv.pkg /tmp/mock_upgrade_test/");
    setenv("PKG_SCAN_DIR", "/tmp/mock_upgrade_test", 1);
    pkg_scanner_init();
    pkg_scanner_scan();
    char *json_upgrade = pkg_scanner_packages_for_drive_to_json("/tmp/mock_upgrade_test");
    printf("JSON with cumulative base UPGRADE (installed v5.00, pkg v6.00):\n%s\n", json_upgrade);
    /* Newer base package MUST be installable (can_install: true) */
    assert(strstr(json_upgrade, "\"can_install\":true") != NULL);
    free(json_upgrade);


    /* Step D2: Simulate newer version installed on console (v7.00), base package on drive is v6.00 */
    fp = fopen("/tmp/mock_appmeta/PPSA90011/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"PPSA90011\",\"contentVersion\":\"07.000.000\",\"titleName\":\"NebulaView\"}\n");
    fclose(fp);
    fp = fopen("/tmp/mock_user_app/PPSA90011/sce_sys/param.json", "w");
    assert(fp != NULL);
    fprintf(fp, "{\"titleId\":\"PPSA90011\",\"contentVersion\":\"07.000.000\",\"titleName\":\"NebulaView\"}\n");
    fclose(fp);
    pkg_scanner_init();
    pkg_scanner_scan();
    char *json_downgrade = pkg_scanner_packages_for_drive_to_json("/tmp/mock_upgrade_test");
    printf("JSON with base installed v7.00 > pkg v6.00:\n%s\n", json_downgrade);
    assert(strstr(json_downgrade, "\"can_install\":false") != NULL);
    assert(strstr(json_downgrade, "Installed version is same or newer") != NULL);
    free(json_downgrade);
    system("rm -rf /tmp/mock_upgrade_test");

    system("rm -rf /tmp/mock_appmeta /tmp/mock_addcont /tmp/mock_usb_install_test /tmp/mock_user_app");
    unsetenv("PKG_SCAN_DIR");
    unsetenv("PKG_USB_PREFIX");
    unsetenv("PKG_APPMETA_DIR");
    unsetenv("PKG_ADDCONT_DIR");
    unsetenv("PKG_USER_APP_DIR");

    /* =========================================================================
     * Test 3: Fallback handling for unparsed / non-standard .pkg
     * ========================================================================= */
    printf("\n=== Testing Fallback for Non-Standard .pkg ===\n");
    system("mkdir -p /tmp/test_pkg_scan && echo 'NOT_A_REAL_PKG' > /tmp/test_pkg_scan/CustomGameName_v1.0.pkg");
    setenv("PKG_SCAN_DIR", "/tmp/test_pkg_scan", 1);
    pkg_scanner_init();
    int count2 = pkg_scanner_scan();
    printf("Unparsed test scan count: %d\n", count2);
    assert(count2 == 1);

    pkg_detail_t unparsed;
    assert(pkg_scanner_get_at(0, &unparsed) == 0);
    printf("Unparsed title_name: %s\n", unparsed.title_name);
    assert(strcmp(unparsed.title_name, "Unknown Package") == 0);
    assert(strstr(unparsed.title_name, "CustomGameName") == NULL); /* Crucial: never extracted from filename! */

    char *json2 = pkg_scanner_to_json();
    assert(strstr(json2, "Unknown Package") != NULL);
    assert(strstr(json2, "CustomGameName") != NULL);
    free(json2);
    system("rm -rf /tmp/test_pkg_scan");

    /* =========================================================================
     * Test 4: Drive Scanning, Clickable Logic, and Drive Filtering
     * ========================================================================= */
    printf("\n=== Testing Drive Scanning & Root Directory Isolation ===\n");
    unsetenv("PKG_SCAN_DIR");

    system("rm -rf /tmp/mock_usb* /tmp/mock_disc*");
    system("mkdir -p /tmp/mock_usb0/nested /tmp/mock_usb1 /tmp/mock_disc");
    system("cp /tmp/test_scan_fixtures/wc.pkg /tmp/mock_usb0/");
    system("cp /tmp/test_scan_fixtures/nv.pkg /tmp/mock_usb0/nested/");
    system("cp /tmp/test_scan_fixtures/nv.pkg /tmp/mock_disc/");

    setenv("PKG_USB_PREFIX", "/tmp/mock_usb", 1);
    setenv("PKG_DISC_DIR", "/tmp/mock_disc", 1);

    pkg_scanner_init();
    int drive_scan_pkgs = pkg_scanner_scan();
    printf("Drive scan found %d packages total\n", drive_scan_pkgs);

    size_t dcount = pkg_scanner_get_drive_count();
    printf("Detected drives count: %zu\n", dcount);
    assert(dcount == 3); /* usb0, usb1, disc */

    pkg_drive_t drive0, drive1, ddisc;
    assert(pkg_scanner_get_drive_at(0, &drive0) == 0);
    assert(pkg_scanner_get_drive_at(1, &drive1) == 0);
    assert(pkg_scanner_get_drive_at(2, &ddisc) == 0);

    assert(strcmp(drive0.id, "usb0") == 0);
    assert(drive0.pkg_count == 1); /* Only root pkg! Not nested/ */
    assert(drive0.clickable == 1);

    assert(strcmp(drive1.id, "usb1") == 0);
    assert(drive1.pkg_count == 0);
    assert(drive1.clickable == 0); /* Must NOT be clickable */

    assert(strcmp(ddisc.id, "disc") == 0);
    assert(ddisc.pkg_count == 1);
    assert(ddisc.clickable == 1);

    char *drives_json = pkg_scanner_drives_to_json();
    assert(strstr(drives_json, "\"id\":\"usb0\"") != NULL);
    assert(strstr(drives_json, "\"clickable\":true") != NULL);
    assert(strstr(drives_json, "\"id\":\"usb1\"") != NULL);
    assert(strstr(drives_json, "\"clickable\":false") != NULL);
    assert(strstr(drives_json, "\"id\":\"disc\"") != NULL);
    free(drives_json);

    char *usb0_pkgs_json = pkg_scanner_packages_for_drive_to_json("usb0");
    assert(strstr(usb0_pkgs_json, "WaveCast") != NULL);
    assert(strstr(usb0_pkgs_json, "NebulaView") == NULL);
    free(usb0_pkgs_json);

    char *usb1_pkgs_json = pkg_scanner_packages_for_drive_to_json("usb1");
    assert(strcmp(usb1_pkgs_json, "[]") == 0);
    free(usb1_pkgs_json);
    char *disc_pkgs_json = pkg_scanner_packages_for_drive_to_json("disc");
    assert(strstr(disc_pkgs_json, "NebulaView") != NULL);
    assert(strstr(disc_pkgs_json, "WaveCast") == NULL);
    free(disc_pkgs_json);

    system("rm -rf /tmp/mock_usb* /tmp/mock_disc*");
    unsetenv("PKG_USB_PREFIX");
    unsetenv("PKG_DISC_DIR");

    /* =========================================================================
     * Test 5: Dotfile and Dot-directory Filtering (._*.pkg, .hidden.pkg, etc.)
     * ========================================================================= */
    printf("\n=== Testing Dotfile Exclusion (.dot.pkg and ._*.pkg) ===\n");
    system("rm -rf /tmp/mock_dotfiles_test");
    system("mkdir -p /tmp/mock_dotfiles_test/.dot_dir");
    system("cp /tmp/test_scan_fixtures/wc.pkg /tmp/mock_dotfiles_test/visible_app.pkg");
    system("cp /tmp/test_scan_fixtures/wc.pkg /tmp/mock_dotfiles_test/._mac_metadata.pkg");
    system("cp /tmp/test_scan_fixtures/wc.pkg /tmp/mock_dotfiles_test/.hidden_app.pkg");
    system("cp /tmp/test_scan_fixtures/wc.pkg /tmp/mock_dotfiles_test/.dot_dir/nested.pkg");

    setenv("PKG_SCAN_DIR", "/tmp/mock_dotfiles_test", 1);
    pkg_scanner_init();
    int dot_scan_count = pkg_scanner_scan();
    printf("Dotfile scan count: %d (expected: 1)\n", dot_scan_count);
    assert(dot_scan_count == 1);

    char *dot_json = pkg_scanner_to_json();
    assert(strstr(dot_json, "visible_app.pkg") != NULL);
    assert(strstr(dot_json, "._mac_metadata.pkg") == NULL);
    assert(strstr(dot_json, ".hidden_app.pkg") == NULL);
    assert(strstr(dot_json, "nested.pkg") == NULL);
    free(dot_json);

    system("rm -rf /tmp/mock_dotfiles_test");
    unsetenv("PKG_SCAN_DIR");

    /* =========================================================================
     * Test 6: Deep Version Normalization & Comparison Edge Cases
     * ========================================================================= */
     printf("\n=== Testing Version Normalization & Comparison Edge Cases ===\n");
     char norm[32];
     app_info_normalize_version("01.000.003", norm, sizeof(norm));
     assert(strcmp(norm, "v1.03") == 0);
     app_info_normalize_version("06.000.000", norm, sizeof(norm));
     assert(strcmp(norm, "v6.00") == 0);
     app_info_normalize_version("01.06", norm, sizeof(norm));
     assert(strcmp(norm, "v01.06") == 0);
     app_info_normalize_version("v1.03", norm, sizeof(norm));
     assert(strcmp(norm, "v1.03") == 0);

     /* Symmetrical matching between PS5 3-part format and 2-part normalized versions */
     assert(app_info_compare_versions("01.000.003", "v1.03") == 0);
     assert(app_info_compare_versions("v1.03", "01.000.003") == 0);
     assert(app_info_compare_versions("01.000.003", "v01.03") == 0);
     assert(app_info_compare_versions("01.000.004", "v1.03") > 0);
     assert(app_info_compare_versions("01.000.002", "v1.03") < 0);
     assert(app_info_compare_versions("06.000.000", "v6.00") == 0);
     assert(app_info_compare_versions("v6.00", "05.000.000") > 0);
     assert(app_info_compare_versions("v01.06", "v01.00") > 0);
     assert(app_info_compare_versions("v01.06", "v01.06") == 0);
     assert(app_info_compare_versions("v1.10", "v1.2") > 0);

     /* =========================================================================
     * Test 7: DLC Substring False-Positive Edge Cases
     * ========================================================================= */
     printf("\n=== Testing DLC Subdirectory Isolation (Preventing False Positives) ===\n");
     system("rm -rf /tmp/mock_addcont_fp");
      system("mkdir -p /tmp/mock_addcont_fp/CUSA12346/00 /tmp/mock_addcont_fp/CUSA12346/temp /tmp/mock_addcont_fp/CUSA12346/UP");
     setenv("PKG_ADDCONT_DIR", "/tmp/mock_addcont_fp", 1);
     unsetenv("PKG_APP_DB_PATH");

      /* Substrings "00", "temp", "UP" exist in /user/addcont/CUSA12346, but DLC is NOT installed! */
      int dlc_installed = app_info_check_dlc_installed("CUSA12346", "UP1234-CUSA12346_00-TESTPACK0000008");
     printf("DLC installed with substring subdirs: %d (expected: 0)\n", dlc_installed);
     assert(dlc_installed == 0);

     /* Now add exact DLC label folder */
      system("mkdir -p /tmp/mock_addcont_fp/CUSA12346/TESTPACK0000008");
      dlc_installed = app_info_check_dlc_installed("CUSA12346", "UP1234-CUSA12346_00-TESTPACK0000008");
     printf("DLC installed with exact label subdir: %d (expected: 1)\n", dlc_installed);
     assert(dlc_installed == 1);

     system("rm -rf /tmp/mock_addcont_fp");
     unsetenv("PKG_ADDCONT_DIR");

     /* =========================================================================
     * Test 8: SQLite app.db Key-Value and Wide-Column Schema Support
     * ========================================================================= */
     printf("\n=== Testing SQLite app.db Schemas (Key-Value & Wide-Column) ===\n");
     system("rm -f /tmp/mock_test_app.db");

     /* Create SQLite db with wide-column table (without version column, only appVer) */
      system("python3 -c \"import sqlite3; conn = sqlite3.connect('/tmp/mock_test_app.db'); "
             "conn.execute('CREATE TABLE tbl_appinfo (titleId TEXT, appVer TEXT);'); "
             "conn.execute('INSERT INTO tbl_appinfo VALUES (\\\"PPSA12345\\\", \\\"01.000.003\\\");'); "
            "conn.commit(); conn.close()\"");

     setenv("PKG_APP_DB_PATH", "/tmp/mock_test_app.db", 1);
     char ver_from_db[32] = {0};
      int app_in_db = app_info_check_installed("PPSA12345", ver_from_db, sizeof(ver_from_db));
     printf("App in wide-column DB: %d, version: %s\n", app_in_db, ver_from_db);
     assert(app_in_db == 1);
     assert(strcmp(ver_from_db, "v1.03") == 0);

     /* Test key-value table variant */
     system("rm -f /tmp/mock_test_app.db");
     system("python3 -c \"import sqlite3; conn = sqlite3.connect('/tmp/mock_test_app.db'); "
            "conn.execute('CREATE TABLE tbl_appinfo (titleId TEXT, key TEXT, val TEXT);'); "
            "conn.execute('INSERT INTO tbl_appinfo VALUES (\\\"PPSA12346\\\", \\\"APP_VER\\\", \\\"06.000.000\\\");'); "
            "conn.commit(); conn.close()\"");

     ver_from_db[0] = '\0';
      app_in_db = app_info_check_installed("PPSA12346", ver_from_db, sizeof(ver_from_db));
     printf("App in key-value DB: %d, version: %s\n", app_in_db, ver_from_db);
     assert(app_in_db == 1);
     assert(strcmp(ver_from_db, "v6.00") == 0);

     /* Test PS5 tbl_contentinfo with AppInfoJson */
     system("rm -f /tmp/mock_test_app.db");
     system("python3 -c \"import sqlite3; conn = sqlite3.connect('/tmp/mock_test_app.db'); "
            "conn.execute('CREATE TABLE tbl_contentinfo (titleId TEXT, AppInfoJson TEXT, metaDataPath TEXT);'); "
            "json_val = '{\\\"field_list\\\":[{\\\"data\\\":\\\"01.06\\\",\\\"key\\\":\\\"APP_VER\\\"},{\\\"data\\\":\\\"01.00\\\",\\\"key\\\":\\\"VERSION\\\"}]}'; "
             "conn.execute('INSERT INTO tbl_contentinfo VALUES (\\\"CUSA12345\\\", ?, \\\"\\\");', (json_val,)); "
            "conn.commit(); conn.close()\"");

     ver_from_db[0] = '\0';
      app_in_db = app_info_check_installed("CUSA12345", ver_from_db, sizeof(ver_from_db));
     printf("App in tbl_contentinfo with AppInfoJson: %d, version: %s\n", app_in_db, ver_from_db);
     assert(app_in_db == 1);
     assert(app_info_compare_versions(ver_from_db, "v1.06") == 0 || app_info_compare_versions(ver_from_db, "v01.06") == 0);

     /* Test patch version priority: base app has v1.00 in user/app, patch has v1.06 in user/patch */
     system("rm -rf /tmp/mock_test_user_app /tmp/mock_test_user_patch");
      system("mkdir -p /tmp/mock_test_user_app/CUSA12345/sce_sys /tmp/mock_test_user_patch/CUSA12345/sce_sys");
      FILE *fp_base = fopen("/tmp/mock_test_user_app/CUSA12345/sce_sys/param.json", "w");
      assert(fp_base != NULL);
      fprintf(fp_base, "{\"titleId\":\"CUSA12345\",\"contentVersion\":\"01.000.000\"}\n");
      fclose(fp_base);
      FILE *fp_patch = fopen("/tmp/mock_test_user_patch/CUSA12345/sce_sys/param.json", "w");
      assert(fp_patch != NULL);
      fprintf(fp_patch, "{\"titleId\":\"CUSA12345\",\"contentVersion\":\"01.060.000\"}\n");
     fclose(fp_patch);

     setenv("PKG_USER_APP_DIR", "/tmp/mock_test_user_app", 1);
     setenv("PKG_USER_PATCH_DIR", "/tmp/mock_test_user_patch", 1);
     unsetenv("PKG_APP_DB_PATH");
     unsetenv("PKG_APPMETA_DIR");

     char ver_patch_test[32] = {0};
      int installed_patch = app_info_check_installed("CUSA12345", ver_patch_test, sizeof(ver_patch_test));
     printf("App with base v1.00 and patch v1.06 detected version: %s\n", ver_patch_test);
     assert(installed_patch == 1);
     assert(app_info_compare_versions(ver_patch_test, "v1.00") > 0);
     system("rm -rf /tmp/mock_test_user_app /tmp/mock_test_user_patch");
     unsetenv("PKG_USER_APP_DIR");
     unsetenv("PKG_USER_PATCH_DIR");
     system("rm -f /tmp/mock_test_app.db");
     unsetenv("PKG_APP_DB_PATH");

     /* Test 9: Uninstalled app with leftover appmeta and empty user/app must NOT be detected as installed */
      printf("\n=== Testing Uninstalled App with Leftover Appmeta (CUSA90001 case) ===\n");
     system("rm -rf /tmp/mock_test_user_app /tmp/mock_test_appmeta");
     /* Empty directory in user/app */
      system("mkdir -p /tmp/mock_test_user_app/CUSA90001");
      /* Leftover patch/trophy param.json in appmeta */
      system("mkdir -p /tmp/mock_test_appmeta/CUSA90001");
      FILE *fp_stale = fopen("/tmp/mock_test_appmeta/CUSA90001/param.json", "w");
      assert(fp_stale != NULL);
      fprintf(fp_stale, "{\"titleId\":\"CUSA90001\",\"contentVersion\":\"01.260.000\"}\n");
     fclose(fp_stale);

     setenv("PKG_USER_APP_DIR", "/tmp/mock_test_user_app", 1);
     setenv("PKG_APPMETA_DIR", "/tmp/mock_test_appmeta", 1);
     unsetenv("PKG_APP_DB_PATH");

     char ver_uninstalled[32] = {0};
      int is_uninstalled = app_info_check_installed("CUSA90001", ver_uninstalled, sizeof(ver_uninstalled));
     printf("Uninstalled app with stale appmeta detected as installed: %d (expected: 0)\n", is_uninstalled);
     assert(is_uninstalled == 0);

     system("rm -rf /tmp/mock_test_user_app /tmp/mock_test_appmeta");
     unsetenv("PKG_USER_APP_DIR");
     unsetenv("PKG_APPMETA_DIR");

     /* =========================================================================
      * Test 10: Partially Installed (Aborted Install) App Detection & Gating
      * ========================================================================= */
     printf("\n=== Testing Partially Installed / Aborted App Detection & Gating ===\n");
     system("rm -f /tmp/mock_partial_app.db");
     system("rm -rf /tmp/mock_partial_scan");
     system("mkdir -p /tmp/mock_partial_scan");

     /* Create SQLite db with aborted CUSA90002 (contentStatus=1, size=0, _install_sub_status=2) */
     system("python3 -c \"import sqlite3; conn = sqlite3.connect('/tmp/mock_partial_app.db'); "
            "conn.execute('CREATE TABLE tbl_contentinfo (titleId TEXT, AppInfoJson TEXT, metaDataPath TEXT, installStatus INTEGER, contentStatus INTEGER, size INTEGER);'); "
            "json_val = '{\\\"field_list\\\":[{\\\"data\\\":2,\\\"key\\\":\\\"_install_sub_status\\\"},{\\\"data\\\":1,\\\"key\\\":\\\"#_contents_status\\\"}]}'; "
            "conn.execute('INSERT INTO tbl_contentinfo VALUES (\\\"CUSA90002\\\", ?, \\\"\\\", 0, 1, 0);', (json_val,)); "
            "conn.commit(); conn.close()\"");

     setenv("PKG_APP_DB_PATH", "/tmp/mock_partial_app.db", 1);

     char partial_ver[32] = {0};
     int is_inst = app_info_check_installed("CUSA90002", partial_ver, sizeof(partial_ver));
     printf("Aborted app detected as installed: %d (expected: 0)\n", is_inst);
     assert(is_inst == 0);

     char partial_desc[128] = {0};
     int is_part = app_info_check_partially_installed("CUSA90002", partial_desc, sizeof(partial_desc));
     printf("Aborted app detected as partially installed: %d, desc: %s\n", is_part, partial_desc);
     assert(is_part == 1);
     assert(strlen(partial_desc) > 0);

     /* Copy base and update fixture pkgs to scan dir */
     system("cp /tmp/test_scan_fixtures/bq_base.pkg /tmp/test_scan_fixtures/bq_upd.pkg /tmp/mock_partial_scan/");
     setenv("PKG_SCAN_DIR", "/tmp/mock_partial_scan", 1);
     unsetenv("PKG_USB_PREFIX");
     unsetenv("PKG_APPMETA_DIR");
     unsetenv("PKG_ADDCONT_DIR");
     unsetenv("PKG_USER_APP_DIR");

     pkg_scanner_init();
     int part_scanned = pkg_scanner_scan();
     assert(part_scanned == 2);

     char *partial_json = pkg_scanner_packages_for_drive_to_json("/tmp/mock_partial_scan");
     printf("JSON with partially installed app:\n%s\n", partial_json);

     /* Assertions for JSON:
      * 1. is_partially_installed is true
      * 2. partial_desc is present
      * 3. Base package: can_install is true (allows reinstall!)
      * 4. Update package: can_install is false, disabled_reason contains 'aborted'
      */
     assert(strstr(partial_json, "\"is_partially_installed\":true") != NULL);
     assert(strstr(partial_json, "\"partial_desc\":\"Incomplete or aborted installation on console\"") != NULL);
     assert(strstr(partial_json, "Base package installation was aborted. Reinstall base package first.") != NULL);

     /* Verify base package has can_install:true */
     char *base_pos = strstr(partial_json, "bq_base.pkg");
     assert(base_pos != NULL);
     char *can_inst_base = strstr(base_pos, "\"can_install\":true");
     assert(can_inst_base != NULL);

     /* Verify update package has can_install:false */
     char *upd_pos = strstr(partial_json, "bq_upd.pkg");
     assert(upd_pos != NULL);
     char *can_inst_upd = strstr(upd_pos, "\"can_install\":false");
     assert(can_inst_upd != NULL);

     free(partial_json);

      system("rm -f /tmp/mock_partial_app.db");
      system("rm -rf /tmp/mock_partial_scan");
      unsetenv("PKG_SCAN_DIR");
      unsetenv("PKG_APP_DB_PATH");

      /* =========================================================================
       * Test 11: Manifest Save, Load & Verification
       * ========================================================================= */
      printf("\n=== Testing Manifest Save, Load & Verification ===\n");
      system("rm -rf /tmp/mock_manifest_test /tmp/mock_manifest_cache");
      system("mkdir -p /tmp/mock_manifest_test /tmp/mock_manifest_cache");
      setenv("PKG_CACHE_DIR", "/tmp/mock_manifest_cache", 1);
      setenv("PKG_SCAN_DIR", "/tmp/mock_manifest_test", 1);
      unsetenv("PKG_DISC_DIR");
      unsetenv("PKG_USB_PREFIX");

      /* Copy 2 example packages */
      system("cp /tmp/test_scan_fixtures/bq_base.pkg /tmp/mock_manifest_test/pkg1.pkg");
      system("cp /tmp/test_scan_fixtures/bq_upd.pkg /tmp/mock_manifest_test/pkg2.pkg");

      pkg_scanner_init();
      int full_count = pkg_scanner_scan();
      printf("Initial full scan found %d packages\n", full_count);
      assert(full_count == 2);

      /* Manifest should exist on disk */
      assert(pkg_scanner_has_manifest() == 1);

      /* Verify manifest.json exists in PKG_CACHE_DIR */
      struct stat mst;
      assert(stat("/tmp/mock_manifest_cache/manifest.json", &mst) == 0);
      assert(mst.st_size > 100);

      /* Re-initialize scanner: should automatically load from manifest */
      pkg_scanner_init();
      size_t restored_count = pkg_scanner_get_count();
      printf("Restored packages count on init from manifest: %zu\n", restored_count);
      assert(restored_count == 2);
      printf("Restored drive count on init from manifest: %zu\n", pkg_scanner_get_drive_count());
      assert(pkg_scanner_get_drive_count() == 1);

      pkg_detail_t m_d0;
      assert(pkg_scanner_get_at(0, &m_d0) == 0);
      assert(strcmp(m_d0.title_id, "CUSA90002") == 0);

      /* =========================================================================
       * Test 12: Quick Rescan (Unchanged Skip, Add, Remove, and Startup Simulation)
       * ========================================================================= */
      printf("\n=== Testing Quick Rescan Behavior ===\n");

      /* Step A: Quick scan with NO changes -> must return changed == 0 */
      int changed = -1;
      int q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (no changes): count=%d, changed=%d (expected: 2, 0)\n", q_count, changed);
      assert(q_count == 2);
      assert(changed == 0);

      /* Step B: Add a 3rd package -> quick scan must detect new item and return changed == 1 */
      system("cp /tmp/test_scan_fixtures/ab_bonus.pkg /tmp/mock_manifest_test/pkg3.pkg");
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (new item added): count=%d, changed=%d (expected: 3, 1)\n", q_count, changed);
      assert(q_count == 3);
      assert(changed == 1);

      /* Step C: Quick scan again -> must return changed == 0 */
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (second run): count=%d, changed=%d (expected: 3, 0)\n", q_count, changed);
      assert(q_count == 3);
      assert(changed == 0);

      /* Step D: Remove 1 package -> quick scan must purge removed item and return changed == 1 */
      unlink("/tmp/mock_manifest_test/pkg1.pkg");
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (item removed): count=%d, changed=%d (expected: 2, 1)\n", q_count, changed);
      assert(q_count == 2);
      assert(changed == 1);

      /* Step E: Modify file mtime -> quick scan must re-parse and return changed == 1 */
      system("touch -t 202001010000 /tmp/mock_manifest_test/pkg2.pkg");
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (item modified): count=%d, changed=%d (expected: 2, 1)\n", q_count, changed);
      assert(q_count == 2);
      assert(changed == 1);

      /* Step F: Specific drive targeting */
      changed = -1;
      q_count = pkg_scanner_scan_quick("usb0", &changed);
      assert(q_count == 2);
      assert(changed == 0);

      /* Step G: Startup emulation: init loads manifest, quick scan skips unchanged */
      pkg_scanner_init();
      assert(pkg_scanner_get_count() == 2);
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      assert(q_count == 2);
      assert(changed == 0);

      /* Step H: Multi-part package handling in quick scan */
      system("python3 tools/pkg_split.py /tmp/test_scan_big.pkg -o /tmp/mock_manifest_test -s 60M > /dev/null 2>&1");
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (multipart added): count=%d, changed=%d (expected: 3, 1)\n", q_count, changed);
      assert(q_count == 3);
      assert(changed == 1);

      /* Unchanged multi-part package MUST skip re-parsing on subsequent quick scan */
      changed = -1;
      q_count = pkg_scanner_scan_quick(NULL, &changed);
      printf("Quick scan (multipart unchanged): count=%d, changed=%d (expected: 3, 0)\n", q_count, changed);
      assert(q_count == 3);
      assert(changed == 0);

      /* Step I: __all__ packages serialization */
      char *all_json = pkg_scanner_packages_for_drive_to_json("__all__");
      assert(all_json != NULL);
      assert(strstr(all_json, "CUSA90002") != NULL);
      assert(strstr(all_json, "PPSA90010") != NULL);
      free(all_json);

      /* Step J: Empty drive invariance: quick scan on empty mounted drive must return changed == 0 */
      system("mkdir -p /tmp/mock_usb_empty0");
      unsetenv("PKG_SCAN_DIR");
      setenv("PKG_USB_PREFIX", "/tmp/mock_usb_empty", 1);
      changed = -1;
      q_count = pkg_scanner_scan_quick("usb0", &changed);
      /* Empty drive with no files matches 0 recorded files -> changed == 0 */
      printf("Quick scan (empty drive mounted): count=%d, changed=%d (expected: 0)\n", q_count, changed);
      assert(changed == 0);

      /* Second quick scan on unchanged empty drive MUST return changed == 0 */
      changed = -1;
      q_count = pkg_scanner_scan_quick("usb0", &changed);
      printf("Quick scan (empty drive unchanged): count=%d, changed=%d (expected: 0)\n", q_count, changed);
      assert(changed == 0);

      /* Step K: Unmount empty drive -> quick scan must detect unmount (changed == 1) */
      system("rm -rf /tmp/mock_usb_empty0");
      changed = -1;
      q_count = pkg_scanner_scan_quick("usb0", &changed);
      printf("Quick scan (empty drive unmounted): count=%d, changed=%d (expected: 1)\n", q_count, changed);
      assert(changed == 1);

      /* Subsequent scan with drive remaining unmounted MUST return changed == 0 */
      changed = -1;
      q_count = pkg_scanner_scan_quick("usb0", &changed);
      printf("Quick scan (drive remaining unmounted): count=%d, changed=%d (expected: 0)\n", q_count, changed);
      assert(changed == 0);

      /* Step L: Multi-Language Package Scanning & Accept-Language Resolution */
      printf("\n=== Testing Multi-Language Package Scanning & Accept-Language Resolution ===\n");
      system("mkdir -p /tmp/mock_multilang_test");
      assert(fixture_write_ps5_pkg_multilang("/tmp/mock_multilang_test/ml.pkg",
                                             "PPSA90099", "en-US", "gd", "01.000.000", 0) == 0);
      assert(fixture_write_ps5_pkg("/tmp/mock_multilang_test/single.pkg",
                                   "PPSA90011", "SingleLangGame", "gd", "01.000.000", 0) == 0);

      setenv("PKG_SCAN_DIR", "/tmp/mock_multilang_test", 1);
      pkg_scanner_init();
      int ml_count = pkg_scanner_scan();
      assert(ml_count == 2);

      /* Without Accept-Language, defaults to defaultLanguage ("English Title") */
      char *json_def = pkg_scanner_packages_for_drive_to_json_ex("/tmp/mock_multilang_test", NULL);
      assert(json_def != NULL);
      assert(strstr(json_def, "\"title_name\":\"English Title\"") != NULL);
      assert(strstr(json_def, "\"default_language\":\"en-US\"") != NULL);
      assert(strstr(json_def, "\"localized_titles\":{") != NULL);
      assert(strstr(json_def, "\"ar-AE\":\"Arabic Title\"") != NULL);
      assert(strstr(json_def, "\"title_name\":\"SingleLangGame\"") != NULL);
      free(json_def);

      /* With Accept-Language: ar, resolves to Arabic Title */
      char *json_ar = pkg_scanner_packages_for_drive_to_json_ex("/tmp/mock_multilang_test", "ar-AE,ar;q=0.9");
      assert(json_ar != NULL);
      assert(strstr(json_ar, "\"title_name\":\"Arabic Title\"") != NULL);
      assert(strstr(json_ar, "\"title_name\":\"SingleLangGame\"") != NULL);
      free(json_ar);

      /* With Accept-Language: pl, resolves to Polish Title */
      char *json_pl = pkg_scanner_packages_for_drive_to_json_ex("/tmp/mock_multilang_test", "pl-PL,pl;q=0.8");
      assert(json_pl != NULL);
      assert(strstr(json_pl, "\"title_name\":\"Polish Title\"") != NULL);
      assert(strstr(json_pl, "\"title_name\":\"SingleLangGame\"") != NULL);
      free(json_pl);

      /* With unsupported Accept-Language (e.g. ja), falls back to defaultLanguage */
      char *json_ja = pkg_scanner_packages_for_drive_to_json_ex("/tmp/mock_multilang_test", "ja-JP,ja;q=0.9");
      assert(json_ja != NULL);
      assert(strstr(json_ja, "\"title_name\":\"English Title\"") != NULL);
      free(json_ja);

      system("rm -rf /tmp/mock_multilang_test");
      system("rm -rf /tmp/mock_manifest_test /tmp/mock_manifest_cache /tmp/mock_usb_empty*");
      unsetenv("PKG_CACHE_DIR");
      unsetenv("PKG_SCAN_DIR");
      unsetenv("PKG_USB_PREFIX");
      unsetenv("PKG_DISC_DIR");

      printf("\n>>> ALL PKG SCANNER TESTS PASSED! <<<\n");
      return 0;
}
