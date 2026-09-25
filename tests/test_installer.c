#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "installer.h"
#include "pkg_scanner.h"
#include "test_fixture.h"

/* Wait up to timeout_ms for cond_fn-style polling via status snapshot. */
static int wait_for_state(int want_installing, int want_completed, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        installer_status_t st;
        installer_get_status(&st);
        if (!!st.is_installing == !!want_installing &&
            (!want_completed || st.completed)) {
            return 0;
        }
        usleep(100000);
        waited += 100;
    }
    return -1;
}

int main(void) {
    int res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    installer_status_t st;
    installer_get_status(&st);
    assert(st.is_installing == 0);
    assert(strcmp(st.status_str, "idle") == 0);

    char *json = installer_status_to_json();
    printf("Initial status JSON: %s\n", json);
    assert(strstr(json, "\"is_installing\":false") != NULL);
    free(json);

    /* Synthetic fixture: no external files needed. */
    system("rm -rf /tmp/test_installer_fixtures && mkdir -p /tmp/test_installer_fixtures");
    assert(fixture_write_ps5_pkg("/tmp/test_installer_fixtures/wc.pkg",
                                 "PPSA90012", "WaveCast", "gd", "01.003.000", 1) == 0);

    /* Test 0: Missing package file fails immediately with -4 */
    int missing_res = installer_start("/tmp/test_installer_fixtures/missing_nonexistent.pkg");
    printf("Missing pkg install start result: %d (expected -4)\n", missing_res);
    assert(missing_res == -4);
    installer_get_status(&st);
    assert(st.is_installing == 0);

    /* Test 1: Start install with standard pkg (streams via worker thread) */
    int start_res = installer_start("/tmp/test_installer_fixtures/wc.pkg");
    printf("Start install result: %d\n", start_res);
    assert(start_res == 0);

    /* Title fields are populated synchronously by installer_start */
    installer_get_status(&st);
    assert(strcmp(st.title_id, "PPSA90012") == 0);
    assert(strcmp(st.title_name, "WaveCast") == 0);

    /* The worker streams to completion asynchronously; wait for it */
    assert(wait_for_state(0, 1, 15000) == 0);
    installer_get_status(&st);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.status_str, "playable") == 0);
    printf("Single-pkg stream install completed: progress=%.1f%%\n", st.progress_percent);

    char *active_json = installer_status_to_json();
    printf("Final status JSON: %s\n", active_json);
    assert(strstr(active_json, "WaveCast") != NULL);
    assert(strstr(active_json, "PPSA90012") != NULL);
    free(active_json);

    /* Test 1b: the native installer owns the base -> update handoff. This
     * must complete without any status polling or browser-side callback. */
    assert(fixture_write_ps5_pkg("/tmp/test_installer_fixtures/wc_update.pkg",
                                 "PPSA90012", "WaveCast", "gp", "01.004.000", 1) == 0);
    int batch_res = installer_start_batch("/tmp/test_installer_fixtures/wc.pkg",
                                          "/tmp/test_installer_fixtures/wc_update.pkg");
    printf("Start native base + update batch result: %d\n", batch_res);
    assert(batch_res == 0);

    int batch_waited = 0;
    int update_seen = 0;
    while (batch_waited < 15000) {
        installer_get_status(&st);
        if (strcmp(st.pkg_path, "/tmp/test_installer_fixtures/wc_update.pkg") == 0) {
            update_seen = 1;
        }
        if (update_seen && !st.is_installing && st.completed) break;
        usleep(100000);
        batch_waited += 100;
    }
    assert(update_seen == 1);
    assert(st.completed == 1);
    assert(st.failed == 0);
    assert(strcmp(st.pkg_path, "/tmp/test_installer_fixtures/wc_update.pkg") == 0);
    printf("Native base + update batch completed without browser polling\n");

    installer_shutdown();

    /* Test 2: Monitor passivity: no browser relaunch machinery remains.
     * The installer monitor worker is intentionally passive so someone can
     * play a title while a big package installs in the background. */
    printf("Testing monitor passivity (install must keep running)...\n");
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);
    system("rm -f /tmp/watchdog_big.pkg && truncate -s 3G /tmp/watchdog_big.pkg");
    int big_res = installer_start("/tmp/watchdog_big.pkg");
    assert(big_res == 0);

    sleep(8); /* Exceed old 7-second threshold without polling */

    installer_get_status(&st);
    assert(st.is_installing == 1); /* sparse 3GB stream still running */
    assert(installer_cancel() == 0);
    installer_shutdown();
    system("rm -f /tmp/watchdog_big.pkg");

    /* Test 3: Fallback install on an unparsable/raw file, through to completion */
    res = installer_init("http://127.0.0.1:8085/");
    assert(res == 0);

    system("echo 'dummy content' > /tmp/dummy_test.pkg");
    int unparsed_res = installer_start("/tmp/dummy_test.pkg");
    printf("Unparsed pkg install start result: %d\n", unparsed_res);
    assert(unparsed_res == 0); /* MUST succeed and not return -3 */

    assert(wait_for_state(0, 1, 10000) == 0);
    char *unparsed_json = installer_status_to_json();
    printf("Unparsed status JSON: %s\n", unparsed_json);
    assert(strstr(unparsed_json, "Package") != NULL);
    assert(strstr(unparsed_json, "\"completed\":true") != NULL);
    free(unparsed_json);

    installer_shutdown();
    system("rm -f /tmp/dummy_test.pkg");

    printf("\n>>> ALL INSTALLER TESTS PASSED! <<<\n");
    return 0;
}
