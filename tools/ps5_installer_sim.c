/*
 * PS5 Installer Stream Simulator — command-line client half.
 *
 * Points the same PS5 installer request-pattern replay (ps5_sim.c) at a live
 * stream server. Used two ways:
 *
 *   1. Standalone against an already-running server on the host:
 *        tools/ps5_installer_sim --no-server --host 127.0.0.1 --port 8845
 *
 *   2. Self-contained: build a fixture PKG, start the real stream_server.c on
 *      the host, replay the PS5 pattern, and print a reference-format replay
 *      log + summary (the "mock the PS5 on the PC" setup, no PS5 required).
 *
 * This is the shape a future websocket / remote install method needs: swap the
 * raw-socket client for a websocket client and the same request sequence runs.
 */

#include "ps5_sim.h"
#include "stream_server.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define DEFAULT_SESSION "package-test-1.pkg"
#define DEFAULT_CONTENT_ID "EP9000-PPSA90012_00-TESTSIM00000001"
#define FIX_DIR   "/tmp/ps5_installer_sim"
#define FIX_PKG   FIX_DIR "/pkg.pkg"
#define DEFAULT_SIZE (5ULL * 1024 * 1024) /* 5 MiB: header + bulk chunks */

typedef struct {
    const char *pkg;           /* fixture path; NULL => build default fixture */
    const char *host;
    int         port;
    const char *session;
    const char *content_id;

    int    header_repeats;
    int    reqs_per_header;
    int    parallel;
    uint64_t chunk_size;
    uint64_t size;

    int    no_server;          /* connect to an existing server, don't start one */
    int    demo;               /* write a reference-format replay log */
    const char *replay_path;   /* replay log path (NULL => derive) */
} opts_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "PS5 Installer Stream Simulator\n"
        "usage: %s [options]\n"
        "  --pkg FILE         PKG to serve (built as a fixture if omitted)\n"
        "  --host HOST        Stream server host (default 127.0.0.1)\n"
        "  --port PORT        Stream server port (default %d)\n"
        "  --session NAME     Session name (default %s)\n"
        "  --content-id ID    Content id for the .crc sidecar probe\n"
        "  --header-repeats N Header-phase connections (default 5)\n"
        "  --reqs-per-header  Requests per header connection (default 2)\n"
        "  --parallel N       Bulk parallel connections (default 2)\n"
        "  --chunk-size N     Bulk chunk size in bytes (default 16 MiB)\n"
        "  --size N           Fixture size in bytes (default 5 MiB)\n"
        "  --no-server        Connect to an existing server instead of starting one\n"
        "  --demo             Write a reference-format replay log\n"
        "  --replay PATH      Replay log path (default ./replay.log)\n",
        prog, STREAM_SERVER_PORT, DEFAULT_SESSION);
}

/* Build a small synthetic PS5 PKG fixture so there is something to stream. */
static void build_fixture(const char *path, uint64_t size) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%.*s", (int)(sizeof(dir) - 1), path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), "%s", FIX_DIR);
    }
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "WARNING: failed to prepare fixture dir %s\n", dir);
    }

    assert(fixture_write_ps5_pkg(path, "PPSA90012", "SimGame", "gd",
                                 "01.000.000", 1) == 0);
    assert(fixture_grow_file(path, size) == 0);
}

int main(int argc, char **argv) {
    opts_t o;
    memset(&o, 0, sizeof(o));
    o.host = "127.0.0.1";
    o.port = STREAM_SERVER_PORT;
    o.session = DEFAULT_SESSION;
    o.content_id = DEFAULT_CONTENT_ID;
    o.header_repeats = 5;
    o.reqs_per_header = 2;
    o.parallel = 2;
    o.chunk_size = PS5_SIM_DEFAULT_CHUNK;
    o.size = DEFAULT_SIZE;
    o.pkg = NULL;
    o.demo = 0;
    o.replay_path = "replay.log";

    for (int i = 1; i < argc; i++) {
#define STRCMP_OPT(name) (strcmp(argv[i], name) == 0)
        if (STRCMP_OPT("--help") || STRCMP_OPT("-h")) { usage(argv[0]); return 0; }
        else if (STRCMP_OPT("--no-server")) { o.no_server = 1; }
        else if (STRCMP_OPT("--demo")) { o.demo = 1; }
        else if (STRCMP_OPT("--host") && i + 1 < argc) { o.host = argv[++i]; }
        else if (STRCMP_OPT("--port") && i + 1 < argc) { o.port = atoi(argv[++i]); }
        else if (STRCMP_OPT("--session") && i + 1 < argc) { o.session = argv[++i]; }
        else if (STRCMP_OPT("--content-id") && i + 1 < argc) { o.content_id = argv[++i]; }
        else if (STRCMP_OPT("--header-repeats") && i + 1 < argc) { o.header_repeats = atoi(argv[++i]); }
        else if (STRCMP_OPT("--reqs-per-header") && i + 1 < argc) { o.reqs_per_header = atoi(argv[++i]); }
        else if (STRCMP_OPT("--parallel") && i + 1 < argc) { o.parallel = atoi(argv[++i]); }
        else if (STRCMP_OPT("--chunk-size") && i + 1 < argc) { o.chunk_size = strtoull(argv[++i], NULL, 10); }
        else if (STRCMP_OPT("--size") && i + 1 < argc) { o.size = strtoull(argv[++i], NULL, 10); }
        else if (STRCMP_OPT("--pkg") && i + 1 < argc) { o.pkg = argv[++i]; }
        else if (STRCMP_OPT("--replay") && i + 1 < argc) { o.replay_path = argv[++i]; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 2; }
#undef STRCMP_OPT
    }

    const char *path = o.pkg ? o.pkg : FIX_PKG;
    if (!o.no_server) {
        if (!o.pkg) build_fixture(path, o.size);
        printf("=== PS5 Installer Stream Simulator ===\n");
        printf("serving %s (%llu bytes) -> session '%s' on %s:%d\n",
               path, (unsigned long long)o.size, o.session, o.host, o.port);
        assert(stream_server_session_start_ex(path, o.session) == 0);
        assert(stream_server_is_running() == 1);
    } else {
        printf("=== PS5 Installer Stream Simulator (no local server) ===\n");
        printf("replaying against %s:%d session '%s'\n", o.host, o.port, o.session);
    }

    FILE *replay = NULL;
    if (o.demo) {
        replay = fopen(o.replay_path, "w");
        if (!replay) { perror("fopen replay.log"); return 1; }
    }

    char pathbuf[512];
    snprintf(pathbuf, sizeof(pathbuf), "/stream/install/%s", o.session);

    ps5_sim_target_t base;
    memset(&base, 0, sizeof(base));
    base.host = o.host;
    base.port = o.port;
    base.path = pathbuf;
    base.content_id = o.content_id;
    base.check_crc = 1;
    base.header_repeats = o.header_repeats;
    base.reqs_per_header = o.reqs_per_header;
    base.parallel = o.parallel;
    base.chunk_size = o.chunk_size;

    ps5_sim_stats_t st;
    int rc = ps5_sim_replay(&base, &st, replay);
    if (replay) fclose(replay);

    if (o.demo) {
        printf("\n----- replay log (%s) -----\n", o.replay_path);
        FILE *fp = fopen(o.replay_path, "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) fputs(line, stdout);
            fclose(fp);
        } else {
            perror("fopen replay.log (read)");
        }
    }

    printf("\n----- summary -----\n");
    printf("replay_rc=%d  connections=%d  requests=%u  bytes_delivered=%llu\n",
           rc, st.conns_opened, (unsigned)st.requests_made,
           (unsigned long long)st.total_delivered);
    printf("header_ok=%d  bulk_ok=%d  crc_404=%d  failures=%d\n",
           st.header_ok, st.bulk_ok, st.crc_404, st.failures);

    if (!o.no_server) {
        stream_server_session_stop();
        assert(stream_server_is_running() == 0);
    }

    if (rc == 0 && st.failures == 0) {
        printf("\n>>> SIM COMPLETE: PS5 request pattern served cleanly <<<\n");
        return 0;
    }
    printf("\n>>> SIM FAILED <<<\n");
    return 1;
}
