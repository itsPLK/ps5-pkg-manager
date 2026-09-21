/*
 * ws_push_sim — host CLI that pushes a file to a ws_direct live session,
 * exactly like the DirectInstallView browser page does (WS handshake, text
 * init, fragmented binary, finish). Two modes:
 *
 *   1. Against a live server (C daemon or frontend/mock-server.js):
 *        tools/ws_push_sim --pkg game.pkg --host 127.0.0.1 --port 8846
 *
 *   2. Self-contained --demo: build a fixture PKG, start a live RAM
 *      session + listener + the real stream server on the host, push over
 *      a real socket on a thread while the main thread replays the PS5
 *      pull pattern (ps5_sim) concurrently. No PS5, no disk spool.
 */

#include "ws_upload.h"
#include "ws_stream.h"
#include "ws_test_client.h"
#include "stream_server.h"
#include "ps5_sim.h"
#include "test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#define DEMO_DIR   "/tmp/ws_push_sim"
#define DEMO_SRC   DEMO_DIR "/src.pkg"
#define DEMO_SIZE  (5ULL * 1024 * 1024)
#define DEMO_SESSION "package-ws-push-1.pkg"
#define DEMO_PATH    "/stream/install/package-ws-push-1.pkg"

typedef struct {
    const char *pkg;
    const char *host;
    int port;
    unsigned long chunk;
    uint64_t throttle_bps; /* 0 = unthrottled */
    long abort_at;         /* >=0: close without finish after this many bytes */
    int fragmented;        /* split each chunk into ~128 KiB frames (Chrome) */
    int demo;
    int resume_test;
} opts_t;

static void usage(const char *prog) {
    fprintf(stderr,
        "ws_push_sim — push a file to a ws_direct live session\n"
        "usage: %s [options]\n"
        "  --pkg FILE       file to push (built as fixture with --demo)\n"
        "  --host HOST      upload host (default 127.0.0.1)\n"
        "  --port PORT      WS listener port (default %d)\n"
        "  --chunk-size N   chunk bytes per message (default 1 MiB)\n"
        "  --throttle BPS   pace the push (producer-slower-than-consumer)\n"
        "  --abort-at N     drop the connection after N bytes (no finish)\n"
        "  --fragmented     split messages like Chrome (~128 KiB frames)\n"
        "  --resume-test    drop mid-upload, reconnect and resume\n"
        "  --demo           self-contained live push + concurrent PS5 pull\n",
        prog, WS_DIRECT_DEFAULT_PORT);
}

typedef struct {
    const opts_t *o;
    const char *pkg_path;
    uint64_t total;
    int rc;
} push_job_t;

static void pace(const opts_t *o, size_t n, struct timespec *t0, uint64_t sent) {
    if (!o->throttle_bps) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double want = (double)(sent + n) / (double)o->throttle_bps;
    double have = (now.tv_sec - t0->tv_sec) + (now.tv_nsec - t0->tv_nsec) / 1e9;
    if (want > have) usleep((useconds_t)((want - have) * 1e6));
}

#define SEG (WS_LIVE_SEG_SIZE)

/* Push one connection of whole segments from resume_seg; returns segments
 * newly sent, or -1 on error. Stops at abort_at (no finish: the resume
 * test reconnects and continues). Every message carries its seg header. */
static long push_conn(int fd, FILE *f, uint64_t total, uint64_t resume_seg,
                      const opts_t *o, struct timespec *t0) {
    uint64_t nsegs = (total + SEG - 1) / SEG;
    uint64_t abort_seg = (o->abort_at >= 0) ? (uint64_t)o->abort_at / SEG : nsegs;
    unsigned char *buf = malloc(SEG);
    if (!buf) return -1;
    char rep[2048];
    char segmsg[64];
    long sent_segs = 0;
    for (uint64_t seg = resume_seg; seg < nsegs; seg++) {
        uint64_t off = seg * SEG;
        size_t want = SEG;
        if (off + want > total) want = (size_t)(total - off);
        if (seg >= abort_seg) break;
        if (fseek(f, (long)off, SEEK_SET) != 0) { free(buf); return -1; }
        if (fread(buf, 1, want, f) != want) { free(buf); return -1; }
        snprintf(segmsg, sizeof(segmsg), "{\"op\":\"seg\",\"seg\":%llu}",
                 (unsigned long long)seg);
        if (ws_client_send_text(fd, segmsg) != 0) { free(buf); return -1; }
        if (o->fragmented) {
            /* Chrome-style: first frame + ~128 KiB continuations. */
            size_t foff = 0;
            int first = 1;
            while (foff < want) {
                size_t fn = want - foff > 130933 ? 130933 : want - foff;
                int fin = (foff + fn == want);
                if (ws_client_send_frame(fd, first ? 0x2 : 0x0, fin,
                                         buf + foff, fn) != 0) {
                    free(buf);
                    return -1;
                }
                first = 0;
                foff += fn;
            }
        } else {
            if (ws_client_send_binary(fd, buf, want) != 0) { free(buf); return -1; }
        }
        pace(o, want, t0, (uint64_t)sent_segs * SEG);
        if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0) { free(buf); return -1; }
        if (strstr(rep, "\"ack\"") == NULL) {
            fprintf(stderr, "expected ack, got: %s\n", rep);
            free(buf);
            return -1;
        }
        sent_segs++;
    }
    free(buf);
    return sent_segs;
}

static int run_push(const opts_t *o, const char *pkg_path, uint64_t total) {
    FILE *f = fopen(pkg_path, "rb");
    if (!f) { perror("fopen pkg"); return 1; }
    const char *base = strrchr(pkg_path, '/');
    base = base ? base + 1 : pkg_path;
    char init[512];
    snprintf(init, sizeof(init), "{\"op\":\"init\",\"filename\":\"%s\",\"total\":%llu}",
             base, (unsigned long long)total);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int fd = ws_client_connect(o->host, o->port, "/ws/upload");
    if (fd < 0) { fprintf(stderr, "WS connect failed\n"); fclose(f); return 1; }
    if (ws_client_send_text(fd, init) != 0) { fclose(f); return 1; }
    char rep[2048] = {0};
    if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0 || !strstr(rep, "\"ready\"")) {
        fprintf(stderr, "no ready: %s\n", rep);
        fclose(f);
        return 1;
    }
    printf("ready: %s\n", rep);

    uint64_t nsegs = (total + SEG - 1) / SEG;
    long sent = push_conn(fd, f, total, 0, o, &t0);
    if (sent < 0) { fclose(f); return 1; }
    if (o->resume_test || (o->abort_at >= 0 && (uint64_t)sent < nsegs)) {
        /* Drop + reconnect: same file+total resumes at the server offset. */
        printf("dropped at %ld segs, reconnecting...\n", sent);
        ws_client_close(fd);
        fd = ws_client_connect(o->host, o->port, "/ws/upload");
        if (fd < 0) { fclose(f); return 1; }
        if (ws_client_send_text(fd, init) != 0) { fclose(f); return 1; }
        if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0 || !strstr(rep, "\"ready\"")) {
            fprintf(stderr, "no ready on resume: %s\n", rep);
            fclose(f);
            return 1;
        }
        printf("resumed: %s\n", rep);
        const char *op = strstr(rep, "\"offset\":");
        uint64_t from = op ? strtoull(op + 9, NULL, 10) / SEG : 0;
        opts_t o2 = *o;
        o2.abort_at = -1;
        o2.resume_test = 0;
        if (push_conn(fd, f, total, from, &o2, &t0) < 0) { fclose(f); return 1; }
    }

    if (ws_client_send_text(fd, "{\"op\":\"finish\"}") != 0) { fclose(f); return 1; }
    if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0 || !strstr(rep, "\"complete\"")) {
        fprintf(stderr, "no complete: %s\n", rep);
        fclose(f);
        return 1;
    }
    printf("complete: %s\n", rep);
    ws_client_close(fd);
    fclose(f);
    return 0;
}

static void *push_thread_main(void *arg) {
    push_job_t *j = (push_job_t *)arg;
    j->rc = run_push(j->o, j->pkg_path, j->total);
    return NULL;
}

int main(int argc, char **argv) {
    opts_t o;
    memset(&o, 0, sizeof(o));
    o.host = "127.0.0.1";
    o.port = WS_DIRECT_DEFAULT_PORT;
    o.chunk = 1024 * 1024;
    o.abort_at = -1;

    for (int i = 1; i < argc; i++) {
#define OPT(n) (strcmp(argv[i], n) == 0)
        if (OPT("--help") || OPT("-h")) { usage(argv[0]); return 0; }
        else if (OPT("--demo")) { o.demo = 1; }
        else if (OPT("--resume-test")) { o.resume_test = 1; }
        else if (OPT("--fragmented")) { o.fragmented = 1; }
        else if (OPT("--host") && i + 1 < argc) { o.host = argv[++i]; }
        else if (OPT("--port") && i + 1 < argc) { o.port = atoi(argv[++i]); }
        else if (OPT("--chunk-size") && i + 1 < argc) { o.chunk = strtoul(argv[++i], NULL, 10); }
        else if (OPT("--throttle") && i + 1 < argc) { o.throttle_bps = strtoull(argv[++i], NULL, 10); }
        else if (OPT("--abort-at") && i + 1 < argc) { o.abort_at = atol(argv[++i]); }
        else if (OPT("--pkg") && i + 1 < argc) { o.pkg = argv[++i]; }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(argv[0]); return 2; }
#undef OPT
    }
    if (o.chunk == 0 || o.chunk > 8 * 1024 * 1024) {
        fprintf(stderr, "chunk-size must be 1..8MiB\n");
        return 2;
    }

    if (o.demo) {
        printf("=== ws_push_sim DEMO (live, concurrent push+pull) ===\n");
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", DEMO_DIR, DEMO_DIR);
        assert(system(cmd) == 0);
        assert(fixture_write_ps5_pkg(DEMO_SRC, "PPSA90012", "PushGame", "gd",
                                     "01.000.000", 1) == 0);
        assert(fixture_grow_file(DEMO_SRC, DEMO_SIZE) == 0);
        ws_live_reset_for_tests();
        ws_live_set_timeout_sec(60);
        /* Live session + listener first (push thread + pull share it). */
        setenv("WS_DIRECT_PORT", "18847", 1);
        char sid[64] = {0};
        /* Same basename the push thread will send in its WS init. */
        assert(ws_direct_init_session("src.pkg", DEMO_SIZE, sid, sizeof(sid)) == 0);
        const int port = 18847;
        o.port = port;
        o.host = "127.0.0.1";
        assert(ws_direct_listener_running());
        opts_t po = o;
        /* Demo push mirrors a browser: fragmented + mildly throttled so
         * readers genuinely block mid-stream. */
        po.fragmented = 1;
        if (!po.throttle_bps) po.throttle_bps = 20ULL * 1024 * 1024;
        push_job_t job = { &po, DEMO_SRC, DEMO_SIZE, -1 };
        pthread_t pth;
        assert(pthread_create(&pth, NULL, push_thread_main, &job) == 0);

        /* Concurrent pull: serve live: while pushing. */
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(stream_server_session_start_ex(uri, DEMO_SESSION) == 0);
        ps5_sim_target_t t;
        memset(&t, 0, sizeof(t));
        t.host = "127.0.0.1";
        t.port = STREAM_SERVER_PORT;
        t.path = DEMO_PATH;
        t.content_id = "EP9000-PPSA90012_00-TESTPUSH000001";
        t.check_crc = 1;
        t.header_repeats = 3;
        t.reqs_per_header = 2;
        t.parallel = 2;
        t.chunk_size = PS5_SIM_DEFAULT_CHUNK;
        t.total_size = DEMO_SIZE;
        ps5_sim_stats_t st;
        int rc = ps5_sim_replay(&t, &st, NULL);
        printf("pull replay_rc=%d header_ok=%d bulk_ok=%d crc_404=%d failures=%d\n",
               rc, st.header_ok, st.bulk_ok, st.crc_404, st.failures);
        void *prc = NULL;
        pthread_join(pth, &prc);
        int push_rc = job.rc;
        printf("push_rc=%d\n", push_rc);
        stream_server_session_stop();
        ws_direct_listener_stop();
        ws_direct_reset_for_tests();
        if (rc == 0 && !st.failures && push_rc == 0) {
            printf("\n>>> PUSH+PULL DEMO COMPLETE <<<\n");
            return 0;
        }
        printf("\n>>> DEMO FAILED <<<\n");
        return 1;
    }

    if (!o.pkg) { usage(argv[0]); return 2; }
    struct stat st;
    if (stat(o.pkg, &st) != 0) { perror("stat pkg"); return 1; }
    printf("=== ws_push_sim: pushing %s (%lld bytes) to %s:%d ===\n",
           o.pkg, (long long)st.st_size, o.host, o.port);
    return run_push(&o, o.pkg, (uint64_t)st.st_size);
}
