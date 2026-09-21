/*
 * End-to-end PS5 stream-simulation test.
 *
 * Pairs the PS5 installer simulator (ps5_sim.c) against the REAL stream
 * server (stream_server.c) running on the host — no PS5 required. This is
 * the "mock the PS5 on the PC" setup: it proves the server speaks the exact
 * HTTP contract the console expects (206 ranges, strict 404 for the .crc
 * sidecar) and that our replayed request pattern is a valid client.
 *
 * Two modes:
 *   test_stream_sim            run under `make test`; asserts, returns 0/1.
 *   test_stream_sim --demo     build a fixture, replay the PS5 pattern
 *                              against a live server, and print a
 *                              reference-format replay log + summary.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "stream_server.h"
#include "installer.h"
#include "ps5_sim.h"
#include "test_fixture.h"

#define FIX_DIR   "/tmp/test_stream_sim"
#define FIX_PKG   FIX_DIR "/pkg.pkg"
#define SESSION   "package-test-1.pkg"
#define PATH      "/stream/install/package-test-1.pkg"
#define CONTENT_ID "EP9000-PPSA90012_00-TESTSIM00000001"
#define PKG_SIZE  (5ULL * 1024 * 1024) /* 5 MiB: header + bulk chunks */

/* ---- raw GET used for independent byte-integrity checks ----------------- */

static int fetch_range(const char *host, int port, const char *path,
                       uint64_t start, uint64_t end, uint8_t **out_body, size_t *out_len) {
    char range[96];
    snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
             (unsigned long long)start, (unsigned long long)end);
    char req[2048];
    snprintf(req, sizeof(req),
             "GET %s%s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n%s\r\n\r\n",
             path, PS5_SIM_DEFAULT_QUERY, range);

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd >= 0 && connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    /* Read headers byte-by-byte so a response can never have its body bytes
     * absorbed into the header buffer (which would under-read the body past
     * the connection close and give a wrong Content-Length match). */
    char hdr[8192]; size_t hlen = 0;
    send(fd, req, strlen(req), 0);
    for (;;) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) break;
        if (hlen + 1 >= sizeof(hdr)) break;
        hdr[hlen++] = c;
        if (hlen >= 4 &&
            hdr[hlen - 4] == '\r' && hdr[hlen - 3] == '\n' &&
            hdr[hlen - 2] == '\r' && hdr[hlen - 1] == '\n') break;
    }
    uint64_t clen = 0;
    for (char *s = hdr; (s = strstr(s, "\r\n")) != NULL; ) {
        s += 2;
        if (strncasecmp(s, "content-length:", 15) == 0)
            clen = strtoull(s + 15, NULL, 10);
    }
    uint8_t *buf = (uint8_t *)malloc(clen ? clen : 1);
    size_t got = 0;
    while (got < clen) {
        ssize_t r = recv(fd, buf + got, clen - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    *out_body = buf;
    *out_len = clen ? got : 0;
    return 0;
}

static void write_fixture(void) {
    system("rm -rf " FIX_DIR " && mkdir -p " FIX_DIR);
    assert(fixture_write_ps5_pkg(FIX_PKG, "PPSA90012", "SimGame", "gd",
                                 "01.000.000", 1) == 0);
    assert(fixture_grow_file(FIX_PKG, PKG_SIZE) == 0);
}

/* Verify the server serves the fixture bytes for a few offsets. */
static void verify_integrity(void) {
    FILE *f = fopen(FIX_PKG, "rb");
    assert(f);
    uint8_t sample[65536];
    assert(fread(sample, 1, sizeof(sample), f) == sizeof(sample));

    uint8_t *b; size_t len;
    /* Header chunk: server bytes [0, 65536) must match the fixture file. */
    assert(fetch_range("127.0.0.1", STREAM_SERVER_PORT, PATH, 0, 65535, &b, &len) == 0);
    assert(len == 65536 && memcmp(b, sample, 65536) == 0);
    free(b);

    /* First bulk chunk, straight after the header: server bytes
     * [65536, 65536+4096) must match the fixture file at the same offset.
     * (Read the expected bytes from the file rather than past the end of
     * `sample`, which only holds the first 64 KiB.) */
    uint8_t bulk[4096];
    assert(fread(bulk, 1, sizeof(bulk), f) == sizeof(bulk));
    assert(fetch_range("127.0.0.1", STREAM_SERVER_PORT, PATH, 65536, 65536 + 4095, &b, &len) == 0);
    assert(len == 4096 && memcmp(b, bulk, 4096) == 0);
    free(b);

    fclose(f);
}

static int build_target(const ps5_sim_target_t *base, ps5_sim_target_t *t) {
    *t = *base;
    t->host = "127.0.0.1";
    t->port = STREAM_SERVER_PORT;
    t->path = PATH;
    t->content_id = CONTENT_ID;
    t->check_crc = 1;
    t->total_size = PKG_SIZE;
    return 0;
}

/* Run the full PS5 replay and assert the model holds. */
static void run_scenario(const char *name, ps5_sim_target_t base,
                         int expect_fail) {
    ps5_sim_target_t t;
    build_target(&base, &t);
    ps5_sim_stats_t st;
    int rc = ps5_sim_replay(&t, &st, NULL);

    printf("  scenario '%s': replay_rc=%d header_ok=%d bulk_ok=%d crc_404=%d "
           "failures=%d conns=%d reqs=%u bytes=%llu\n",
           name, rc, st.header_ok, st.bulk_ok, st.crc_404, st.failures,
           st.conns_opened, (unsigned)st.requests_made,
           (unsigned long long)st.total_delivered);

    if (expect_fail) {
        assert(rc != 0 || st.failures != 0);
        return;
    }
    assert(rc == 0);
    assert(st.header_ok == 1);
    assert(st.bulk_ok == 1);
    assert(st.crc_404 == 1);
    assert(st.failures == 0);
    assert(st.conns_opened >= base.header_repeats);
}

static void test_end_to_end(void) {
    printf("=== End-to-end: real stream_server.c + simulated PS5 client ===\n");

    write_fixture();

    /* Start the real stream server pinned to our session filename. */
    assert(stream_server_session_start_ex(FIX_PKG, SESSION) == 0);
    assert(stream_server_is_running() == 1);

    /* Sanity: the .crc sidecar must NOT be served (server pins *.pkg name). */
    {
        uint8_t *b; size_t len;
        assert(fetch_range("127.0.0.1", STREAM_SERVER_PORT,
                           "/stream/install/EP9000-PPSA90012_00-TESTSIM00000001.crc",
                           0, 0, &b, &len) == 0);
        assert(len == 0); /* 404 -> empty body */
        free(b);
    }

    /* Scenario 1: default-ish pattern (2 parallel bulk connections). */
    {
        ps5_sim_target_t base;
        memset(&base, 0, sizeof(base));
        base.header_repeats = 5;
        base.reqs_per_header = 2;
        base.parallel = 2;
        base.chunk_size = PS5_SIM_DEFAULT_CHUNK;
        run_scenario("default-2parallel", base, 0);
    }

    /* Scenario 2: single bulk connection (smaller install behavior). */
    {
        ps5_sim_target_t base;
        memset(&base, 0, sizeof(base));
        base.header_repeats = 3;
        base.reqs_per_header = 2;
        base.parallel = 1;
        base.chunk_size = PS5_SIM_DEFAULT_CHUNK;
        run_scenario("single-conn", base, 0);
    }

    /* Scenario 3: minimal header reads (1 repeat, 1 req). */
    {
        ps5_sim_target_t base;
        memset(&base, 0, sizeof(base));
        base.header_repeats = 1;
        base.reqs_per_header = 1;
        base.parallel = 2;
        base.chunk_size = PS5_SIM_DEFAULT_CHUNK;
        run_scenario("minimal-headers", base, 0);
    }

    verify_integrity();

    stream_server_session_stop();
    assert(stream_server_is_running() == 0);
    printf("  -> real stream server passed the PS5 replay and stopped cleanly\n");
}

static void test_missing_server(void) {
    /* Against a closed port the client must report failures (not hang/crash). */
    printf("=== Negative: no server listening (port 1) ===\n");
    ps5_sim_target_t base;
    memset(&base, 0, sizeof(base));
    base.header_repeats = 2;
    base.parallel = 1;
    ps5_sim_target_t t;
    build_target(&base, &t);
    t.host = "127.0.0.1";
    t.port = 1; /* almost certainly closed */
    ps5_sim_stats_t st;
    int rc = ps5_sim_replay(&t, &st, NULL);
    printf("  replay_rc=%d failures=%d (expected nonzero)\n", rc, st.failures);
    assert(rc != 0 || st.failures != 0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* flush progress as we go */
    setvbuf(stderr, NULL, _IOLBF, 0);
    int demo = 0;
    const char *pkg = FIX_PKG;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            demo = 1;
        } else if (strcmp(argv[i], "--pkg") == 0 && i + 1 < argc) {
            pkg = argv[++i];
        }
    }

    if (demo) {
        printf("=== PS5 stream-simulation DEMO (real stream server) ===\n");
        if (strcmp(pkg, FIX_PKG) == 0) write_fixture();

        FILE *replay = fopen("/tmp/test_stream_sim/replay.log", "w");
        assert(replay);

        assert(stream_server_session_start_ex(pkg, SESSION) == 0);

        ps5_sim_target_t base;
        memset(&base, 0, sizeof(base));
        base.header_repeats = 5;
        base.reqs_per_header = 2;
        base.parallel = 2;
        base.chunk_size = PS5_SIM_DEFAULT_CHUNK;
        base.check_crc = 1;
        base.content_id = CONTENT_ID;

        ps5_sim_stats_t st;
        ps5_sim_target_t t;
        build_target(&base, &t);
        ps5_sim_replay(&t, &st, replay);
        fclose(replay);

        printf("\n----- replay log (stream_debug reference format) -----\n");
        FILE *fp = fopen("/tmp/test_stream_sim/replay.log", "r");
        assert(fp);
        char line[1024];
        while (fgets(line, sizeof(line), fp)) fputs(line, stdout);
        fclose(fp);

        printf("\n----- summary -----\n");
        printf("connections=%d  requests=%u  bytes_delivered=%llu  "
               "header_ok=%d  bulk_ok=%d  crc_404=%d  failures=%d\n",
               st.conns_opened, (unsigned)st.requests_made,
               (unsigned long long)st.total_delivered,
               st.header_ok, st.bulk_ok, st.crc_404, st.failures);

        stream_server_session_stop();
        printf("\n>>> DEMO COMPLETE <<<\n");
        return 0;
    }

    printf("==============================================\n");
    printf(">>> RUNNING PS5 STREAM SIMULATION TEST <<<\n");
    printf("==============================================\n");

    test_end_to_end();
    test_missing_server();

    printf("\n>>> ALL STREAM SIMULATION TESTS PASSED! <<<\n");
    return 0;
}
