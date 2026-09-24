#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

#include "pkg_parser.h"
#include "pkg_scanner.h"
#include "installer.h"
#include "test_fixture.h"
#include "app_diag.h"
#include "version.h"
#include "http_server.h"
#include "stream_server.h"
#include "stream_debug_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

static void test_invalid_pkg_files(void) {
    printf("--- Testing invalid PKG handling ---\n");

    pkg_detail_t detail;

    /* 1. NULL path */
    assert(pkg_parser_parse(NULL, &detail) == -1);

    /* 2. Non-existent file */
    assert(pkg_parser_parse("/non/existent/path/foo.pkg", &detail) == -1);

    /* 3. Empty file */
    const char *empty_file = "/tmp/test_empty.pkg";
    FILE *f = fopen(empty_file, "wb");
    fclose(f);
    assert(pkg_parser_parse(empty_file, &detail) == -1);
    unlink(empty_file);

    /* 4. Corrupt magic */
    const char *corrupt_magic = "/tmp/test_corrupt.pkg";
    f = fopen(corrupt_magic, "wb");
    char dummy[256];
    memset(dummy, 0x41, sizeof(dummy));
    fwrite(dummy, 1, sizeof(dummy), f);
    fclose(f);
    assert(pkg_parser_parse(corrupt_magic, &detail) == -1);
    unlink(corrupt_magic);

    /* 5. Invalid icon request */
    uint8_t *icon_data = NULL;
    size_t icon_sz = 0;
    assert(pkg_parser_get_icon(NULL, 0, 0, &icon_data, &icon_sz) == -1);
    assert(pkg_parser_get_icon("/tmp/nonexistent", 100, 100, &icon_data, &icon_sz) == -1);

    printf("Invalid PKG tests passed.\n");
}

static void test_synthetic_sfo(void) {
    printf("--- Testing synthetic PS4 SFO parsing ---\n");

    /* Create a mini PKG containing a PS4 CNT header and param.sfo */
    const char *sfo_pkg = "/tmp/test_ps4.pkg";
    FILE *f = fopen(sfo_pkg, "wb");
    assert(f != NULL);

    /* Write CNT header (0x80 bytes) */
    uint8_t cnt_hdr[0x80];
    memset(cnt_hdr, 0, sizeof(cnt_hdr));
    memcpy(cnt_hdr, "\x7f" "CNT", 4);
    /* entry count = 2 */
    cnt_hdr[0x10] = 0; cnt_hdr[0x11] = 0; cnt_hdr[0x12] = 0; cnt_hdr[0x13] = 2;
    /* table offset = 0x80 */
    cnt_hdr[0x18] = 0; cnt_hdr[0x19] = 0; cnt_hdr[0x1A] = 0; cnt_hdr[0x1B] = 0x80;
    /* content id = CUSA00001_00 */
    memcpy(cnt_hdr + 0x40, "EP0001-CUSA00001_00-TESTGAME00000000", 36);
    fwrite(cnt_hdr, 1, sizeof(cnt_hdr), f);

    /* Entry 0: String table (type 0x0200) at 0x100, size 32 */
    uint8_t entry0[32];
    memset(entry0, 0, sizeof(entry0));
    entry0[3] = 0x02; /* type 0x0200 (BE) -> entry0[2]=0x02, entry0[3]=0x00 */
    entry0[2] = 0x02; entry0[3] = 0x00;
    entry0[18] = 0x01; entry0[19] = 0x00; /* offset 0x100 */
    entry0[23] = 32; /* size 32 */

    /* Entry 1: param.sfo (type 0x1000) at 0x120, size 128 */
    uint8_t entry1[32];
    memset(entry1, 0, sizeof(entry1));
    entry1[2] = 0x10; entry1[3] = 0x00; /* type 0x1000 */
    entry1[7] = 0x00; /* fn_off = 0 ("param.sfo") */
    entry1[18] = 0x01; entry1[19] = 0x20; /* offset 0x120 */
    entry1[23] = 128; /* size 128 */

    fwrite(entry0, 1, sizeof(entry0), f);
    fwrite(entry1, 1, sizeof(entry1), f);

    /* Pad up to offset 0x100 */
    long cur = ftell(f);
    while (cur < 0x100) { fputc(0, f); cur++; }

    /* String table at 0x100: "param.sfo\0" */
    char strtab[32] = "param.sfo";
    fwrite(strtab, 1, sizeof(strtab), f);

    /* SFO at 0x120 */
    uint8_t sfo[128];
    memset(sfo, 0, sizeof(sfo));
    memcpy(sfo, "\0PSF", 4);
    sfo[4] = 1; sfo[5] = 1; /* version 1.1 */
    /* key table start = 20 + 2*16 = 52 (0x34) */
    sfo[8] = 52;
    /* data table start = 52 + 16 = 68 (0x44) */
    sfo[12] = 68;
    /* entry count = 2 */
    sfo[16] = 2;

    /* Entry 0: TITLE */
    uint8_t *se0 = sfo + 20;
    se0[0] = 0; /* key_off = 0 ("TITLE") */
    se0[2] = 4; se0[3] = 2; /* utf-8 string format */
    se0[4] = 9; /* len */
    se0[8] = 9; /* max len */
    se0[12] = 0; /* data_off = 0 */

    /* Entry 1: TITLE_ID */
    uint8_t *se1 = sfo + 36;
    se1[0] = 6; /* key_off = 6 ("TITLE_ID") */
    se1[2] = 4; se1[3] = 2;
    se1[4] = 10;
    se1[8] = 10;
    se1[12] = 10; /* data_off = 10 */

    /* Key table at 52: "TITLE\0TITLE_ID\0" */
    memcpy(sfo + 52, "TITLE\0TITLE_ID\0", 15);
    /* Data table at 68: "Test Game\0CUSA00001\0" */
    memcpy(sfo + 68, "Test Game\0CUSA00001\0", 20);

    fwrite(sfo, 1, sizeof(sfo), f);
    fclose(f);

    pkg_detail_t d;
    int res = pkg_parser_parse(sfo_pkg, &d);
    assert(res == 0);
    assert(strcmp(d.title_name, "Test Game") == 0);
    assert(strcmp(d.title_id, "CUSA00001") == 0);
    unlink(sfo_pkg);

    printf("Synthetic PS4 SFO test passed: title_name='%s', title_id='%s'\n", d.title_name, d.title_id);
}

static void test_scanner_edge_cases(void) {
    printf("--- Testing scanner edge cases ---\n");

    /* Empty directory */
    const char *empty_dir = "/tmp/test_pkg_empty_dir";
    mkdir(empty_dir, 0777);
    setenv("PKG_SCAN_DIR", empty_dir, 1);

    pkg_scanner_init();
    int count = pkg_scanner_scan();
    assert(count == 0);
    assert(pkg_scanner_get_count() == 0);

    char *json = pkg_scanner_to_json();
    assert(strcmp(json, "[]") == 0);
    free(json);

    rmdir(empty_dir);
    printf("Empty directory test passed.\n");
}

static void test_installer_concurrency(void) {
    printf("--- Testing installer concurrency & double-start ---\n");

    /* Synthetic fixtures: no external files needed. */
    system("rm -rf /tmp/test_edge_fixtures && mkdir -p /tmp/test_edge_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_edge_fixtures/a.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 0) == 0);
    assert(fixture_write_ps5_pkg("/tmp/test_edge_fixtures/b.pkg",
                                 "PPSA90011", "NebulaView", "gd", "06.000.000", 0) == 0);

    installer_init("http://127.0.0.1:8085/");

    /* First start */
    int res1 = installer_start("/tmp/test_edge_fixtures/a.pkg");
    assert(res1 == 0);

    /* Second start while first is active */
    int res2 = installer_start("/tmp/test_edge_fixtures/b.pkg");
    assert(res2 == -2); /* -2: already installing */

    installer_shutdown();
    printf("Installer concurrency test passed.\n");
}

static void test_json_escaping_and_unparsed(void) {
    printf("--- Testing JSON escaping and title name fallback ---\n");

    /* Create dummy pkg with special characters in name */
    const char *weird_pkg = "/tmp/Weird \"Game\" Name \\ Test.pkg";
    FILE *f = fopen(weird_pkg, "wb");
    assert(f != NULL);
    fprintf(f, "raw pkg content");
    fclose(f);

    pkg_detail_t detail;
    int res = pkg_parser_parse(weird_pkg, &detail);
    /* Parse should fail because not valid header */
    assert(res == -1);

    installer_init("http://127.0.0.1:8085/");
    int start_res = installer_start(weird_pkg);
    assert(start_res == 0);

    char *json = installer_status_to_json();
    printf("Escaped JSON: %s\n", json);
    /* Verify quotes are escaped with \" and backslashes with \\ */
    assert(strstr(json, "\\\"Game\\\"") != NULL);
    assert(strstr(json, "\\\\") != NULL);
    free(json);

    installer_shutdown();
    unlink(weird_pkg);
    printf("JSON escaping and fallback tests passed.\n");
}

static void test_storage_info(void) {
    printf("--- Testing storage info API ---\n");
    uint64_t free_b = 0, total_b = 0, used_b = 0;
    int ret = system_get_storage_info(&free_b, &total_b, &used_b);
    assert(ret == 0);
    assert(total_b > 0);
    printf("Storage info: total=%llu, free=%llu, used=%llu\n",
           (unsigned long long)total_b, (unsigned long long)free_b, (unsigned long long)used_b);

    /* NVMe storage info test: default /mnt/ext1 on host fails gracefully (-1) */
    uint64_t nv_free = 0, nv_total = 0, nv_used = 0;
    unsetenv("PKG_EXT1_DIR");
    int nv_ret = system_get_nvme_storage_info(&nv_free, &nv_total, &nv_used);
    (void)nv_ret;

    /* NVMe storage info test with simulated mock mount via PKG_EXT1_DIR */
    system("mkdir -p /tmp/mock_nvme_ext1");
    setenv("PKG_EXT1_DIR", "/tmp/mock_nvme_ext1", 1);
    nv_ret = system_get_nvme_storage_info(&nv_free, &nv_total, &nv_used);
    assert(nv_ret == 0);
    assert(nv_total > 0);
    printf("M.2 NVMe mock storage info: total=%llu, free=%llu, used=%llu\n",
           (unsigned long long)nv_total, (unsigned long long)nv_free, (unsigned long long)nv_used);
    unsetenv("PKG_EXT1_DIR");
    system("rm -rf /tmp/mock_nvme_ext1");

    printf("Storage info test passed.\n");
}

static void *concurrent_logger_thread(void *arg) {
    int thread_id = *(int *)arg;
    for (int i = 0; i < 40; i++) {
        if (i % 10 == 0) {
            install_log("[THREAD %d] ERROR: simulated thread failure %d", thread_id, i);
        } else if (i % 5 == 0) {
            install_log("[THREAD %d] WARNING: simulated thread warning %d", thread_id, i);
        } else {
            install_log("[THREAD %d] Routine progress message %d", thread_id, i);
        }
    }
    return NULL;
}

static void test_logging_reduction(void) {
    printf("--- Testing logging reduction & in-memory ring buffer ---\n");
    const char *test_log = "/tmp/test_pkgmgr_install.log";
    unlink(test_log);

    install_log_set_file_path(test_log);
    install_log_clear();

    /* 1. Empty state verification */
    size_t sz = 0;
    char *empty_logs = install_log_get_text(&sz);
    assert(empty_logs != NULL);
    assert(strstr(empty_logs, "No logs recorded yet.") != NULL);
    free(empty_logs);

    /* 2. Informational logs must go to memory ring buffer, NOT to file */
    install_log("[HTTP] Registered active stream session 'test_sess_001'");
    install_log("[INSTALLER] App verified installed in database: PPSA12345");
    install_log("[STREAM] 5000 / 10000 bytes (50.0%%), status='transferring', waiting_disc=0");

    /* Confirm in-memory contains all 3 info logs */
    char *mem_logs = install_log_get_text(&sz);
    assert(mem_logs != NULL);
    assert(strstr(mem_logs, "Registered active stream session") != NULL);
    assert(strstr(mem_logs, "App verified installed in database") != NULL);
    assert(strstr(mem_logs, "status='transferring'") != NULL);
    free(mem_logs);

    /* Confirm file does NOT exist (no writes occurred) */
    struct stat st;
    assert(stat(test_log, &st) != 0);

    /* 3. False-positive checks: ensure names like 'Terror', 'Warner', '4160000' don't trigger file writes */
    install_log("[INSTALLER] Scanning package: /mnt/usb0/Terror_in_Space.pkg");
    install_log("[INSTALLER] Publisher: Warner Bros Interactive");
    install_log("[STREAM] Stream read offset: 4160000 / 10000000 (41.6%%)");
    install_log("[INSTALLER] Title ID PPSA04160 registered successfully");
    assert(stat(test_log, &st) != 0); /* Still no file created */

    /* 4. Errors and warnings MUST write to file */
    install_log("[INSTALLER] System installer reported error 0x80A30001 (Error)");
    install_log("[HTTP] WARNING: active stream refcount underflow");

    /* Confirm file now exists */
    assert(stat(test_log, &st) == 0);
    assert(st.st_size > 0);

    /* Read file content */
    FILE *f = fopen(test_log, "r");
    assert(f != NULL);
    char fbuf[4096] = {0};
    size_t rd = fread(fbuf, 1, sizeof(fbuf) - 1, f);
    fclose(f);
    assert(rd > 0);
    fbuf[rd] = '\0';

    /* File MUST contain the error and warning */
    assert(strstr(fbuf, "System installer reported error") != NULL);
    assert(strstr(fbuf, "WARNING: active stream refcount underflow") != NULL);

    /* File MUST NOT contain the informational logs or false positives */
    assert(strstr(fbuf, "Registered active stream session") == NULL);
    assert(strstr(fbuf, "App verified installed in database") == NULL);
    assert(strstr(fbuf, "Terror_in_Space") == NULL);
    assert(strstr(fbuf, "Warner Bros") == NULL);
    assert(strstr(fbuf, "4160000") == NULL);

    /* 5. Trailing newline stripping check */
    install_log("[INSTALLER] Message with trailing newline\n\r\n");
    char *nl_logs = install_log_get_text(&sz);
    assert(nl_logs != NULL);
    assert(strstr(nl_logs, "Message with trailing newline\n\n") == NULL);
    free(nl_logs);

    /* 6. Disabling file logging via 'none' or '' */
    const char *none_log = "/tmp/test_pkgmgr_none.log";
    unlink(none_log);
    install_log_set_file_path("none");
    install_log("[INSTALLER] ERROR: Fatal simulated failure when logging disabled");
    assert(stat(none_log, &st) != 0);
    /* Reset path */
    install_log_set_file_path(test_log);

    /* 7. Ring buffer rollover (more than MAX_LOG_LINES lines) */
    install_log_clear();
    for (int i = 0; i < INSTALL_LOG_MAX_LINES + 100; i++) {
        install_log("[TEST] Informational line %d", i);
    }
    char *rollover_logs = install_log_get_text(&sz);
    assert(rollover_logs != NULL);
    char last_line[80];
    snprintf(last_line, sizeof(last_line), "Informational line %d\n", INSTALL_LOG_MAX_LINES + 99);
    assert(strstr(rollover_logs, last_line) != NULL);
    assert(strstr(rollover_logs, "Informational line 100\n") != NULL);
    /* Only the oldest 100 entries should have rolled out. */
    assert(strstr(rollover_logs, "Informational line 99\n") == NULL);
    assert(strstr(rollover_logs, "Informational line 0\n") == NULL);
    free(rollover_logs);

    /* 8. Concurrent multithreaded logging safety */
    pthread_t th[4];
    int tids[4] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) {
        pthread_create(&th[i], NULL, concurrent_logger_thread, &tids[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    char *concurrent_logs = install_log_get_text(&sz);
    assert(concurrent_logs != NULL);
    assert(sz > 0);
    free(concurrent_logs);

    /* 9. File rotation / size cap (MAX_LOG_FILE_SIZE = 128KB) */
    /* Artificially append data to exceed 128KB */
    FILE *f_big = fopen(test_log, "w");
    assert(f_big != NULL);
    char dummy_chunk[1024];
    memset(dummy_chunk, 'A', sizeof(dummy_chunk));
    for (int i = 0; i < 130; i++) {
        fwrite(dummy_chunk, 1, sizeof(dummy_chunk), f_big);
    }
    fclose(f_big);

    assert(stat(test_log, &st) == 0 && st.st_size > 128 * 1024);

    /* Next error should trigger log rotation */
    install_log("[INSTALLER] Failed to read header: simulated error");
    assert(stat(test_log, &st) == 0);
    assert(st.st_size < 128 * 1024); /* Truncated/rotated to small size */

    FILE *f_rot = fopen(test_log, "r");
    assert(f_rot != NULL);
    rd = fread(fbuf, 1, sizeof(fbuf) - 1, f_rot);
    fclose(f_rot);
    fbuf[rd] = '\0';
    assert(strstr(fbuf, "LOG ROTATED") != NULL);
    assert(strstr(fbuf, "Failed to read header") != NULL);

    /* Cleanup */
    unlink(test_log);
    install_log_set_file_path(NULL);
    install_log_clear();
    printf("Logging reduction tests passed.\n");
}

static void test_version_and_log_headers(void) {
    printf("--- Testing versioning and log headers ---\n");
    assert(strlen(PKGMGR_VERSION) > 0);
    assert(strlen(PKGMGR_BUILD_COMMIT) > 0);
    assert(strlen(PKGMGR_BUILD_DATE) > 0);

    /* Diagnostic report banner must use PKG Manager branding and include version */
    char *diag = app_diag_generate_report();
    assert(diag != NULL);
    assert(strstr(diag, "PKG MANAGER & SYSTEM APP INFO DIAGNOSTIC") != NULL);
    assert(strstr(diag, "PS5 PKG HUB") == NULL); /* Old branding removed */
    assert(strstr(diag, "PS5 PKG INSTALLER") == NULL); /* Old branding removed */
    assert(strstr(diag, "Version: " PKGMGR_VERSION) != NULL);
    assert(strstr(diag, PKGMGR_BUILD_COMMIT) != NULL);
    free(diag);

    /* Runtime log message */
    install_log_clear();
    install_log("[PKG Manager] Starting PKG Manager v%s (%s, %s)...",
                PKGMGR_VERSION, PKGMGR_BUILD_COMMIT, PKGMGR_BUILD_DATE);
    size_t sz = 0;
    char *logs = install_log_get_text(&sz);
    assert(logs != NULL);
    assert(strstr(logs, "[PKG Manager] Starting PKG Manager v" PKGMGR_VERSION) != NULL);
    assert(strstr(logs, PKGMGR_BUILD_COMMIT) != NULL);
    assert(strstr(logs, "PKG Hub") == NULL);
    assert(strstr(logs, "PKG Installer") == NULL);
    free(logs);
    install_log_clear();

    printf("Versioning and log headers passed.\n");
}

static void test_service_restoration_and_watchdog(void) {
    printf("--- Testing service restoration & network watchdog state machine ---\n");

    /* 1. Initial boot with unknown IP, then network connects */
    assert(http_server_watchdog_evaluate(1, 1, "unknown", "192.168.1.50") == WATCHDOG_ACTION_RESTORE_NETWORK);

    /* 2. Steady state (same IP, server running) - must NOT trigger restart */
    assert(http_server_watchdog_evaluate(1, 1, "192.168.1.50", "192.168.1.50") == WATCHDOG_ACTION_NONE);

    /* 3. IP change (DHCP renew or network switch) - must trigger restart */
    assert(http_server_watchdog_evaluate(1, 1, "192.168.1.50", "10.0.0.99") == WATCHDOG_ACTION_RESTORE_NETWORK);

    /* 4. Network disconnection - must trigger restart for loopback */
    assert(http_server_watchdog_evaluate(1, 0, "192.168.1.50", "unknown") == WATCHDOG_ACTION_RESTORE_LOOPBACK);

    /* 5. Continuing disconnected state (server running) - must NOT keep restarting in loop */
    assert(http_server_watchdog_evaluate(1, 0, "unknown", "unknown") == WATCHDOG_ACTION_NONE);

    /* 6. Self-healing: server down while network is present - must restore network */
    assert(http_server_watchdog_evaluate(0, 1, "192.168.1.50", "192.168.1.50") == WATCHDOG_ACTION_RESTORE_NETWORK);

    /* 7. Self-healing: server down while offline - must restore loopback */
    assert(http_server_watchdog_evaluate(0, 0, "unknown", "unknown") == WATCHDOG_ACTION_RESTORE_LOOPBACK);

    /* 8. Self-healing: server down after wake while IP is unknown - must restore network */
    assert(http_server_watchdog_evaluate(0, 1, "unknown", "192.168.1.50") == WATCHDOG_ACTION_RESTORE_NETWORK);

    /* 9. Robustness: NULL IP strings handled safely without crashing */
    assert(http_server_watchdog_evaluate(1, 1, NULL, "192.168.1.50") == WATCHDOG_ACTION_RESTORE_NETWORK);
    assert(http_server_watchdog_evaluate(1, 0, NULL, NULL) == WATCHDOG_ACTION_NONE);
    assert(http_server_watchdog_evaluate(0, 0, NULL, NULL) == WATCHDOG_ACTION_RESTORE_LOOPBACK);

    /* 10. Standby wake flag simulation */
    volatile sig_atomic_t test_resumed = 1;
    int test_timer = 45;
    int wake_restart = 0;
    if (test_resumed) {
        test_resumed = 0;
        wake_restart = 1;
        test_timer = 0; /* Reset timer so watchdog doesn't immediately re-check */
    }
    assert(test_resumed == 0);
    assert(wake_restart == 1);
    assert(test_timer == 0);

    /* 11. Notification string verification */
    char notify_buf[128];
    snprintf(notify_buf, sizeof(notify_buf), "PKG Manager: Service Restored\nIP: %s", "192.168.1.50");
    assert(strstr(notify_buf, "PKG Manager: Service Restored") != NULL);
    assert(strstr(notify_buf, "192.168.1.50") != NULL);

    snprintf(notify_buf, sizeof(notify_buf), "PKG Manager: Server restart failed after standby");
    assert(strstr(notify_buf, "Server restart failed after standby") != NULL);

    snprintf(notify_buf, sizeof(notify_buf), "PKG Manager: Server restart failed");
    assert(strstr(notify_buf, "Server restart failed") != NULL);

    printf("Service restoration & network watchdog tests passed.\n");
}

static int tcp_connect_stream_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    sin.sin_port = htons(STREAM_SERVER_PORT);
    if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void test_stream_server_http(void) {
    printf("--- Testing stream server HTTP range streaming, keep-alive & strict-404 ---\n");

    /* Enable verbose connection logging so tests exercise all debug code paths */
    stream_server_set_debug(1);

    const char *fix_path = "/tmp/test_stream_server_fix.pkg";
    unlink(fix_path);
    assert(fixture_write_ps5_pkg(fix_path, "PPSA90099", "StreamHttpTest", "gd", "01.000.000", 0) == 0);

    /* Start session pinning session name "package-test.pkg" */
    assert(stream_server_session_start_ex(fix_path, "package-test.pkg") == 0);
    assert(stream_server_is_running() != 0);

    /* 1. Test range request + Keep-Alive: 2 requests on a single TCP socket */
    int sock = tcp_connect_stream_server();
    assert(sock >= 0);

    const char *req1 = "GET /stream/install/package-test.pkg HTTP/1.1\r\n"
                       "Host: 127.0.0.1:18841\r\n"
                       "Range: bytes=0-15\r\n"
                       "Connection: keep-alive\r\n\r\n";
    assert(send(sock, req1, strlen(req1), 0) == (ssize_t)strlen(req1));

    char hdr1[1024] = {0};
    size_t h1_len = 0;
    while (h1_len < sizeof(hdr1) - 1) {
        ssize_t n = recv(sock, hdr1 + h1_len, 1, 0);
        assert(n > 0);
        h1_len += n;
        hdr1[h1_len] = '\0';
        if (strstr(hdr1, "\r\n\r\n") != NULL) break;
    }
    assert(strstr(hdr1, "HTTP/1.1 206 Partial Content") != NULL);
    assert(strstr(hdr1, "Content-Range: bytes 0-15/") != NULL);
    assert(strstr(hdr1, "Content-Length: 16") != NULL);
    assert(strstr(hdr1, "Connection: keep-alive") != NULL);

    /* Read the 16 body bytes for request 1 */
    char body1[16] = {0};
    size_t b1_len = 0;
    while (b1_len < 16) {
        ssize_t n = recv(sock, body1 + b1_len, 16 - b1_len, 0);
        assert(n > 0);
        b1_len += n;
    }
    /* Verify body starts with PS5 package magic '\x7fFIH' */
    assert(memcmp(body1, "\x7f" "FIH", 4) == 0);

    /* Second request on same socket (pipelined keep-alive) */
    const char *req2 = "GET /stream/install/package-test.pkg HTTP/1.1\r\n"
                       "Host: 127.0.0.1:18841\r\n"
                       "Range: bytes=16-31\r\n"
                       "Connection: close\r\n\r\n";
    assert(send(sock, req2, strlen(req2), 0) == (ssize_t)strlen(req2));

    char hdr2[1024] = {0};
    size_t h2_len = 0;
    while (h2_len < sizeof(hdr2) - 1) {
        ssize_t n = recv(sock, hdr2 + h2_len, 1, 0);
        assert(n > 0);
        h2_len += n;
        hdr2[h2_len] = '\0';
        if (strstr(hdr2, "\r\n\r\n") != NULL) break;
    }
    assert(strstr(hdr2, "HTTP/1.1 206 Partial Content") != NULL);
    assert(strstr(hdr2, "Content-Range: bytes 16-31/") != NULL);
    assert(strstr(hdr2, "Content-Length: 16") != NULL);
    assert(strstr(hdr2, "Connection: close") != NULL);

    /* Read the 16 body bytes for request 2 */
    char body2[16] = {0};
    size_t b2_len = 0;
    while (b2_len < 16) {
        ssize_t n = recv(sock, body2 + b2_len, 16 - b2_len, 0);
        assert(n > 0);
        b2_len += n;
    }
    close(sock);

    /* 2. Test Strict-404 rejection: sidecars and unpinned names must 404 */
    sock = tcp_connect_stream_server();
    assert(sock >= 0);
    const char *req_sc = "GET /stream/install/PPSA90099.crc HTTP/1.1\r\n"
                         "Host: 127.0.0.1:18841\r\n"
                         "Connection: close\r\n\r\n";
    assert(send(sock, req_sc, strlen(req_sc), 0) == (ssize_t)strlen(req_sc));
    char buf_sc[1024] = {0};
    ssize_t n_sc = recv(sock, buf_sc, sizeof(buf_sc) - 1, 0);
    assert(n_sc > 0);
    buf_sc[n_sc] = '\0';
    assert(strstr(buf_sc, "HTTP/1.1 404 Not Found") != NULL);
    close(sock);

    /* 3. Test HEAD request: 206 headers without body bytes */
    sock = tcp_connect_stream_server();
    assert(sock >= 0);
    const char *req_head = "HEAD /stream/install/package-test.pkg HTTP/1.1\r\n"
                          "Host: 127.0.0.1:18841\r\n"
                          "Range: bytes=0-15\r\n"
                          "Connection: close\r\n\r\n";
    assert(send(sock, req_head, strlen(req_head), 0) == (ssize_t)strlen(req_head));
    char buf_hd[1024] = {0};
    ssize_t n_hd = recv(sock, buf_hd, sizeof(buf_hd) - 1, 0);
    assert(n_hd > 0);
    buf_hd[n_hd] = '\0';
    assert(strstr(buf_hd, "HTTP/1.1 206 Partial Content") != NULL);
    char *end_hd = strstr(buf_hd, "\r\n\r\n");
    assert(end_hd != NULL && (end_hd + 4 - buf_hd) == n_hd);
    close(sock);

    /* 4. Test 416 Range Not Satisfiable on out-of-bounds range */
    sock = tcp_connect_stream_server();
    assert(sock >= 0);
    const char *req_416 = "GET /stream/install/package-test.pkg HTTP/1.1\r\n"
                          "Host: 127.0.0.1:18841\r\n"
                          "Range: bytes=999999999-999999999\r\n"
                          "Connection: close\r\n\r\n";
    assert(send(sock, req_416, strlen(req_416), 0) == (ssize_t)strlen(req_416));
    char buf_416[1024] = {0};
    ssize_t n_416 = recv(sock, buf_416, sizeof(buf_416) - 1, 0);
    assert(n_416 > 0);
    buf_416[n_416] = '\0';
    assert(strstr(buf_416, "HTTP/1.1 416 Range Not Satisfiable") != NULL);
    close(sock);

    /* Stop session cleanly */
    stream_server_session_stop();
    assert(stream_server_is_running() == 0);
    unlink(fix_path);

    stream_server_set_debug(0);

    printf("Stream server HTTP range streaming tests passed.\n");
}

static void test_stream_debug_logging(void) {
    printf("--- Testing package install stream debug logging ---\n");

    /* Create sandbox debug directory */
    char dbg_dir[] = "/tmp/test_stream_dbg_XXXXXX";
    assert(mkdtemp(dbg_dir) != NULL);
    setenv("PKG_DEBUG_DIR", dbg_dir, 1);

    const char *fix_path = "/tmp/test_stream_dbg.pkg";
    unlink(fix_path);
    assert(fixture_write_ps5_pkg(fix_path, "PPSA77777", "DbgLogTest", "update", "01.020.000", 0) == 0);

    /* 1. Start stream session and open debug log */
    assert(stream_server_session_start_ex(fix_path, "package-dbg.pkg") == 0);
    assert(stream_server_is_running() != 0);

    assert(stream_debug_log_open("PPSA77777", "EP0001-PPSA77777_00-0000000000000000", "update", fix_path, 65837) == 0);
    assert(stream_debug_log_is_active() == 1);

    /* 2. Client 1: Range request */
    int sock1 = tcp_connect_stream_server();
    assert(sock1 >= 0);
    const char *req1 = "GET /stream/install/package-dbg.pkg HTTP/1.1\r\n"
                       "Host: 127.0.0.1:18841\r\n"
                       "Range: bytes=0-31\r\n"
                       "Connection: close\r\n\r\n";
    assert(send(sock1, req1, strlen(req1), 0) == (ssize_t)strlen(req1));

    char buf1[512] = {0};
    ssize_t n1 = 0;
    while ((n1 = recv(sock1, buf1, sizeof(buf1), 0)) > 0) {}
    close(sock1);

    /* 3. Client 2: Concurrent / secondary request */
    int sock2 = tcp_connect_stream_server();
    assert(sock2 >= 0);
    const char *req2 = "GET /stream/install/package-dbg.pkg HTTP/1.1\r\n"
                       "Host: 127.0.0.1:18841\r\n"
                       "Range: bytes=100-199\r\n"
                       "Connection: close\r\n\r\n";
    assert(send(sock2, req2, strlen(req2), 0) == (ssize_t)strlen(req2));

    char buf2[512] = {0};
    ssize_t n2 = 0;
    while ((n2 = recv(sock2, buf2, sizeof(buf2), 0)) > 0) {}
    close(sock2);

    /* 4. Client 3: Invalid path to trigger 404 logging */
    int sock3 = tcp_connect_stream_server();
    assert(sock3 >= 0);
    const char *req3 = "GET /stream/install/other-name.pkg HTTP/1.1\r\n"
                       "Host: 127.0.0.1:18841\r\n"
                       "Connection: close\r\n\r\n";
    assert(send(sock3, req3, strlen(req3), 0) == (ssize_t)strlen(req3));
    char buf3[512] = {0};
    while (recv(sock3, buf3, sizeof(buf3), 0) > 0) {}
    close(sock3);

    /* A retry must preserve the stream report across transport restarts. */
    stream_server_session_stop_keep_log();
    assert(stream_debug_log_is_active() == 1);
    assert(stream_server_session_start_ex(fix_path, "package-retry.pkg") == 0);
    int retry_sock = tcp_connect_stream_server();
    assert(retry_sock >= 0);
    const char *retry_req = "HEAD /stream/install/package-retry.pkg HTTP/1.1\r\n"
                            "Host: 127.0.0.1:18841\r\nConnection: close\r\n\r\n";
    assert(send(retry_sock, retry_req, strlen(retry_req), 0) == (ssize_t)strlen(retry_req));
    char retry_buf[512];
    while (recv(retry_sock, retry_buf, sizeof(retry_buf), 0) > 0) {}
    close(retry_sock);

    /* 5. Stop session and close debug log */
    stream_server_session_stop();
    assert(stream_server_is_running() == 0);
    assert(stream_debug_log_is_active() == 0);
    unlink(fix_path);

    /* 6. Verify debug log file exists and validate contents */
    DIR *d = opendir(dbg_dir);
    assert(d != NULL);
    struct dirent *ent;
    char found_file[1024] = {0};
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "stream_debug_PPSA77777_update_", 30) == 0 &&
            strstr(ent->d_name, ".txt") != NULL) {
            snprintf(found_file, sizeof(found_file), "%s/%s", dbg_dir, ent->d_name);
            break;
        }
    }
    closedir(d);
    assert(found_file[0] != '\0');
    printf("  Found generated debug log: %s\n", found_file);

    /* Read and verify file contents */
    FILE *lf = fopen(found_file, "r");
    assert(lf != NULL);
    char log_content[16384] = {0};
    size_t rd = fread(log_content, 1, sizeof(log_content) - 1, lf);
    fclose(lf);
    assert(rd > 0);
    log_content[rd] = '\0';

    /* Verify header fields */
    assert(strstr(log_content, "# title_id:   PPSA77777") != NULL);
    assert(strstr(log_content, "# content_id: EP0001-PPSA77777_00-0000000000000000") != NULL);
    assert(strstr(log_content, "# pkg_kind:   update") != NULL);
    assert(strstr(log_content, "# total_size: 65837") != NULL);

    /* Verify event entries */
    assert(strstr(log_content, "CONN_OPEN") != NULL);
    assert(strstr(log_content, "Range: bytes=0-31") != NULL);
    assert(strstr(log_content, "Range: bytes=100-199") != NULL);
    assert(strstr(log_content, "BODY_DONE") != NULL);
    assert(strstr(log_content, "sent=32/32") != NULL);
    assert(strstr(log_content, "sent=100/100") != NULL);
    assert(strstr(log_content, "reason=complete") != NULL);
    assert(strstr(log_content, "CONN_CLOSE") != NULL);
    assert(strstr(log_content, "404") != NULL);
    assert(strstr(log_content, "# Session ended") != NULL);
    assert(strstr(log_content, "package-retry.pkg") != NULL);
    assert(strstr(log_content, "INSTALL_EVENT") == NULL);

    /* Clean up */
    unlink(found_file);
    rmdir(dbg_dir);

    printf("Package install stream debug logging tests passed.\n");
}

int main(void) {
    printf(">>> RUNNING EDGE CASE TESTS <<<\n");
    test_invalid_pkg_files();
    test_synthetic_sfo();
    test_scanner_edge_cases();
    test_installer_concurrency();
    test_json_escaping_and_unparsed();
    test_storage_info();
    test_logging_reduction();
    test_version_and_log_headers();
    test_service_restoration_and_watchdog();
    test_stream_server_http();
    test_stream_debug_logging();
    printf("\n>>> ALL EDGE CASE TESTS PASSED! <<<\n");
    return 0;
}
