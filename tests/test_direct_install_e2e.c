/*
 * End-to-end live direct-install test (NEW segment cache + EXISTING
 * serving path). A browser-sim thread pushes over real sockets (baseline
 * cursor + seek detours, Chrome-style fragmentation) while pullers read
 * through the real stream server. Nothing touches disk on the serve path.
 *
 * Scenarios: sequential happy path + integrity, abort fail-fast,
 * minecraft jump (header -> far tail -> sequential bulk with tiny slots,
 * forcing seeks/evictions/resends), installer_start_live commit,
 * parse fallback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "stream_server.h"
#include "installer.h"
#include "multipart.h"
#include "ps5_sim.h"
#include "ws_upload.h"
#include "ws_stream.h"
#include "test_fixture.h"

#define E2E_DIR   "/tmp/test_direct_install"
#define E2E_SRC   E2E_DIR "/src.pkg"
#define CONTENT_ID "EP9000-PPSA90012_00-TESTWS000000001"
#define PKG_SIZE  (5ULL * 1024 * 1024)
#define SESSION   "package-ws-live-1.pkg"
#define SRV_PATH  "/stream/install/package-ws-live-1.pkg"
#define SEG (WS_LIVE_SEG_SIZE)

/* ---------- HTTP range fetch helper ---------- */

static void fetch_range(uint64_t start, uint64_t end, uint8_t *out, size_t len) {
    char range[96];
    snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
             (unsigned long long)start, (unsigned long long)end);
    char req[2048];
    snprintf(req, sizeof(req),
             "GET %s%s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n%s\r\n\r\n",
             SRV_PATH, PS5_SIM_DEFAULT_QUERY, range);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    assert(getaddrinfo("127.0.0.1", "8845", &hints, &res) == 0);
    int fd = -1;
    for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd >= 0 && connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    assert(fd >= 0);
    assert(send(fd, req, strlen(req), 0) == (ssize_t)strlen(req));
    char hdr[4096];
    size_t hlen = 0;
    for (;;) {
        char c;
        assert(recv(fd, &c, 1, 0) == 1);
        if (hlen + 1 < sizeof(hdr)) hdr[hlen++] = c;
        if (hlen >= 4 && !memcmp(hdr + hlen - 4, "\r\n\r\n", 4)) break;
    }
    hdr[hlen] = '\0';
    assert(strstr(hdr, "206") != NULL);
    size_t gotn = 0;
    while (gotn < len) {
        ssize_t r = recv(fd, out + gotn, len - gotn, 0);
        assert(r > 0);
        gotn += (size_t)r;
    }
    close(fd);
}

/* ---------- direct (in-process) sequential pusher ---------- */

typedef struct {
    const char *src;
    uint64_t total;
    size_t slice;
    int pace_us;
    int stop_at;
} push_arg_t;

static void *push_thread(void *arg) {
    push_arg_t *pa = (push_arg_t *)arg;
    FILE *f = fopen(pa->src, "rb");
    if (!f) return (void *)1;
    unsigned char *buf = malloc(pa->slice);
    if (!buf) { fclose(f); return (void *)1; }
    uint64_t nsegs = (pa->total + SEG - 1) / SEG;
    uint64_t stop_seg = (pa->stop_at >= 0) ? (uint64_t)pa->stop_at / SEG : nsegs;
    for (uint64_t seg = 0; seg < nsegs && seg < stop_seg; seg++) {
        uint64_t off = seg * SEG;
        size_t want = SEG;
        if (off + want > pa->total) want = (size_t)(pa->total - off);
        if (fread(buf, 1, want, f) != want) break;
        if (ws_direct_write_chunk(off, buf, want, NULL) != 0) break;
        if (pa->pace_us > 0) usleep(pa->pace_us);
    }
    free(buf);
    fclose(f);
    if (pa->stop_at < 0) ws_direct_finish_session(NULL, 0);
    return (void *)0;
}

/* ---------- socket browser sim (baseline cursor + seek detours) ---------- */

#include "ws_test_client.h"

typedef struct {
    const char *src;
    uint64_t total;
    uint64_t nsegs;
    int fragmented;
    int rc;
    /* When non-NULL, the pusher lingers after all-acked to serve resend
     * seeks until *pull_done is set (mirrors the real browser, which stays
     * connected while an install runs). NULL = finish immediately. */
    volatile int *pull_done;
} browser_arg_t;

static int browser_send_seg(int fd, FILE *f, uint64_t seg, uint64_t total,
                            int fragmented) {
    uint64_t off = seg * SEG;
    size_t len = SEG;
    if (off + len > total) len = (size_t)(total - off);
    unsigned char *buf = malloc(len);
    if (!buf) return -1;
    if (fseek(f, (long)off, SEEK_SET) != 0) { free(buf); return -1; }
    if (fread(buf, 1, len, f) != len) { free(buf); return -1; }
    char segmsg[64];
    snprintf(segmsg, sizeof(segmsg), "{\"op\":\"seg\",\"seg\":%llu}",
             (unsigned long long)seg);
    if (ws_client_send_text(fd, segmsg) != 0) { free(buf); return -1; }
    int rc;
    if (fragmented && len > 130933) {
        /* Chrome-style: first frame + ~128 KiB continuations. */
        size_t foff = 0;
        int first = 1;
        rc = 0;
        while (foff < len) {
            size_t fn = len - foff > 130933 ? 130933 : len - foff;
            int fin = (foff + fn == len);
            if (ws_client_send_frame(fd, first ? 0x2 : 0x0, fin,
                                     buf + foff, fn) != 0) { rc = -1; break; }
            first = 0;
            foff += fn;
        }
    } else {
        rc = ws_client_send_binary(fd, buf, len);
    }
    free(buf);
    return rc;
}

static void *browser_thread(void *arg) {
    browser_arg_t *ba = (browser_arg_t *)arg;
    ba->rc = 1;
    FILE *f = fopen(ba->src, "rb");
    if (!f) return NULL;
    unsigned char *acked = calloc(ba->nsegs ? (size_t)ba->nsegs : 1, 1);
    if (!acked) { fclose(f); return NULL; }

    int fd = ws_client_connect("127.0.0.1", 18847, "/ws/upload");
    if (fd < 0) { fclose(f); free(acked); return NULL; }
    const char *base = strrchr(ba->src, '/');
    base = base ? base + 1 : ba->src;
    char init[512];
    snprintf(init, sizeof(init), "{\"op\":\"init\",\"filename\":\"%s\",\"total\":%llu}",
             base, (unsigned long long)ba->total);
    char rep[2048] = {0};
    if (ws_client_send_text(fd, init) != 0 ||
        ws_client_recv_text(fd, rep, sizeof(rep)) != 0 ||
        !strstr(rep, "\"ready\"")) {
        fclose(f); free(acked);
        return NULL;
    }
    uint64_t cursor = 0;
    {
        const char *op = strstr(rep, "\"offset\":");
        if (op) cursor = strtoull(op + 9, NULL, 10) / SEG;
    }
    /* FIFO seek queue (deduped): two parallel installer workers can park
     * at once -- a scalar detour would drop one of them. */
    long detour_q[16];
    int detour_n = 0;
    uint64_t acked_n = 0;
    for (;;) {
        uint64_t seg;
        if (detour_n > 0) {
            seg = (uint64_t)detour_q[0];
            memmove(detour_q, detour_q + 1,
                    (size_t)--detour_n * sizeof(detour_q[0]));
        } else {
            while (cursor < ba->nsegs && acked[cursor]) cursor++;
            if (cursor >= ba->nsegs) break;
            seg = cursor++;
        }
        if (browser_send_seg(fd, f, seg, ba->total, ba->fragmented) != 0) break;
        /* One outstanding message: wait for its ack (or a seek). A "busy"
         * reply means the ring is momentarily full of unserved data: the
         * segment was NOT stored, so requeue it at the head and retry
         * after a short backoff (never treat as failure). */
        int seg_acked = 0;
        for (;;) {
            if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0) goto done;
            if (strstr(rep, "\"ack\"") != NULL) {
                const char *sp = strstr(rep, "\"seg\":");
                uint64_t a = sp ? strtoull(sp + 6, NULL, 10) : seg;
                if (a < ba->nsegs && !acked[a]) { acked[a] = 1; acked_n++; }
                seg_acked = 1;
                break;
            } else if (strstr(rep, "\"busy\"") != NULL) {
                break; /* not stored: requeue + retry below */
            } else if (strstr(rep, "\"seek\"") != NULL) {
                const char *sp = strstr(rep, "\"seg\":");
                long sq = sp ? (long)strtoull(sp + 6, NULL, 10) : -1;
                if (sq >= 0 && (uint64_t)sq < ba->nsegs && detour_n < 16) {
                    int dup = 0;
                    for (int k = 0; k < detour_n; k++) {
                        if (detour_q[k] == sq) { dup = 1; break; }
                    }
                    if (!dup) detour_q[detour_n++] = sq;
                }
                /* Keep waiting for the ack of the in-flight message. */
            } else {
                goto done; /* error/complete unexpected here */
            }
        }
        if (!seg_acked) {
            if (detour_n < 16) {
                int dup = 0;
                for (int k = 0; k < detour_n; k++) {
                    if (detour_q[k] == (long)seg) { dup = 1; break; }
                }
                if (!dup) {
                    memmove(detour_q + 1, detour_q,
                            (size_t)detour_n * sizeof(detour_q[0]));
                    detour_q[0] = (long)seg;
                    detour_n++;
                }
            }
            usleep(50000); /* match the browser's busy backoff */
        }
    }
    if (acked_n == ba->nsegs) {
        /* The ring is a demand-paged cache: early segments may have been
         * evicted before the puller consumed them. Mirror the real browser
         * (useDirectUpload stays connected while an install runs): linger
         * to serve resend seeks until the puller signals it has drained.
         * Dropped seeks self-heal: the reader re-emits every 2 s while the
         * segment is still missing. */
        if (ba->pull_done) {
            struct timeval linger_tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &linger_tv, sizeof(linger_tv));
            int linger_done = 0;
            while (!linger_done && !*ba->pull_done) {
                if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0) continue;
                if (strstr(rep, "\"seek\"") == NULL) continue;
                const char *sp = strstr(rep, "\"seg\":");
                long s = sp ? (long)strtoull(sp + 6, NULL, 10) : -1;
                if (s < 0 || (uint64_t)s >= ba->nsegs) continue;
                if (browser_send_seg(fd, f, (uint64_t)s, ba->total,
                                     ba->fragmented) != 0) goto done;
                /* Wait for the resend ack (retry on "busy"); later seeks
                 * re-fire via the reader's 2 s re-emit while still missing. */
                for (;;) {
                    if (ws_client_recv_text(fd, rep, sizeof(rep)) != 0) {
                        if (*ba->pull_done) linger_done = 1;
                        break;
                    }
                    if (strstr(rep, "\"ack\"") != NULL) break;
                    if (strstr(rep, "\"busy\"") != NULL) {
                        usleep(50000);
                        if (*ba->pull_done) { linger_done = 1; break; }
                        if (browser_send_seg(fd, f, (uint64_t)s, ba->total,
                                             ba->fragmented) != 0) goto done;
                        continue;
                    }
                    if (*ba->pull_done) { linger_done = 1; break; }
                }
            }
        }
        if (ws_client_send_text(fd, "{\"op\":\"finish\"}") == 0 &&
            ws_client_recv_text(fd, rep, sizeof(rep)) == 0 &&
            strstr(rep, "\"complete\"") != NULL) {
            ba->rc = 0;
        }
    }
done:
    ws_client_close(fd);
    fclose(f);
    free(acked);
    return NULL;
}

/* ---------- minecraft-style pull: header -> far tail -> bulk ---------- */

typedef struct {
    const char *src;
    uint64_t total;
    int rc;
} mc_arg_t;

static void *minecraft_pull_thread(void *arg) {
    mc_arg_t *ma = (mc_arg_t *)arg;
    ma->rc = 1;
    FILE *f = fopen(ma->src, "rb");
    if (!f) return NULL;
    uint8_t *ref = malloc(65536);
    uint8_t *got = malloc(65536);
    if (!ref || !got) return NULL;
    /* Header. */
    assert(fread(ref, 1, 65536, f) == 65536);
    fetch_range(0, 65535, got, 65536);
    if (memcmp(ref, got, 65536) != 0) return NULL;
    /* Far tail jump (last 64 KiB), like the 1029 MB read on a 1 GB file. */
    assert(fseek(f, (long)(ma->total - 65536), SEEK_SET) == 0);
    assert(fread(ref, 1, 65536, f) == 65536);
    fetch_range(ma->total - 65536, ma->total - 1, got, 65536);
    if (memcmp(ref, got, 65536) != 0) return NULL;
    /* Sequential bulk from 65536 with content check. */
    assert(fseek(f, 65536, SEEK_SET) == 0);
    for (uint64_t off = 65536; off < ma->total; off += 65536) {
        size_t n = 65536;
        if (off + n > ma->total) n = (size_t)(ma->total - off);
        if (fread(ref, 1, n, f) != n) return NULL;
        fetch_range(off, off + n - 1, got, n);
        if (memcmp(ref, got, n) != 0) return NULL;
    }
    free(ref);
    free(got);
    fclose(f);
    ma->rc = 0;
    return NULL;
}

/* ---------- console-geometry pull: tail-first parallel bulk, front pivot ----------
 *
 * Replays the production 89.5 MB access pattern: header probes, far
 * probe of seg 56, two parallel tail-bulk workers (segs 56-74 / 74-89),
 * then a front pivot (segs 0-18 / 18-36). The tail bulk evicts the front
 * through far eviction; the pivot must be served through resends. */

typedef struct {
    const char *src;
    uint64_t total;
    int rc;
    int role; /* 0: tail 56-74 + front 0-18; 1: tail 74-89 + front 18-36 */
} pivot_arg_t;

/* Fetch [start, end] in 64 KiB steps and compare against the file.
 * Returns 0 when every byte matches. */
static int pivot_check_range(FILE *f, uint64_t start, uint64_t end) {
    uint8_t *ref = malloc(65536);
    uint8_t *got = malloc(65536);
    if (!ref || !got) { free(ref); free(got); return -1; }
    int rc = -1;
    for (uint64_t off = start; off <= end; ) {
        size_t n = 65536;
        if (off + n - 1 > end) n = (size_t)(end - off + 1);
        if (fseek(f, (long)off, SEEK_SET) != 0) goto out;
        if (fread(ref, 1, n, f) != n) goto out;
        fetch_range(off, off + n - 1, got, n);
        if (memcmp(ref, got, n) != 0) goto out;
        off += n;
    }
    rc = 0;
out:
    free(ref);
    free(got);
    return rc;
}

static void *pivot_reader_thread(void *arg) {
    pivot_arg_t *pa = (pivot_arg_t *)arg;
    pa->rc = 1;
    FILE *f = fopen(pa->src, "rb");
    if (!f) return NULL;
    /* Header probe (seg 0 covers all 0-65535 re-reads). */
    if (pivot_check_range(f, 0, 65535) != 0) { fclose(f); return NULL; }
    if (pa->role == 0) {
        /* Far validation probe: the seg-56 slice from the capture. */
        if (pivot_check_range(f, 58851328, 58916863) != 0) { fclose(f); return NULL; }
        /* Tail bulk, then front pivot. */
        if (pivot_check_range(f, 58916864, 77791231) != 0) { fclose(f); return NULL; }
        if (pivot_check_range(f, 65536, 18939903) != 0) { fclose(f); return NULL; }
    } else {
        if (pivot_check_range(f, 77791232, 91947007) != 0) { fclose(f); return NULL; }
        if (pivot_check_range(f, 91947008, pa->total - 1) != 0) { fclose(f); return NULL; }
        if (pivot_check_range(f, 18939904, 37814271) != 0) { fclose(f); return NULL; }
    }
    fclose(f);
    pa->rc = 0;
    return NULL;
}

static void run_pull(int expect_ok, int chunk, const char *name) {
    ps5_sim_target_t t;
    memset(&t, 0, sizeof(t));
    t.header_repeats = 3;
    t.reqs_per_header = 2;
    t.parallel = 2;
    t.chunk_size = (uint64_t)chunk;
    t.host = "127.0.0.1";
    t.port = STREAM_SERVER_PORT;
    t.path = SRV_PATH;
    t.content_id = CONTENT_ID;
    t.check_crc = 1;
    t.total_size = PKG_SIZE;
    ps5_sim_stats_t st;
    int rc = ps5_sim_replay(&t, &st, NULL);
    printf("  [%s] replay_rc=%d header_ok=%d bulk_ok=%d crc_404=%d failures=%d\n",
           name, rc, st.header_ok, st.bulk_ok, st.crc_404, st.failures);
    if (expect_ok) {
        assert(rc == 0 && st.header_ok && st.bulk_ok && st.crc_404 && !st.failures);
    } else {
        assert(rc != 0 || st.failures != 0);
    }
}

static void write_fixture(void) {
    assert(system("rm -rf " E2E_DIR " && mkdir -p " E2E_DIR) == 0);
    assert(fixture_write_ps5_pkg(E2E_SRC, "PPSA90012", "WsGame", "gd",
                                 "01.000.000", 1) == 0);
    assert(fixture_grow_file(E2E_SRC, PKG_SIZE) == 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("==============================================\n");
    printf(">>> RUNNING DIRECT-INSTALL LIVE E2E TEST <<<\n");
    printf("==============================================\n");
    srand(4242);
    ws_live_set_timeout_sec(30);
    setenv("WS_DIRECT_PORT", "18847", 1);
    write_fixture();

    /* Scenario 1: socket browser push (fragmented) + PS5 pull + integrity. */
    {
        printf("--- scenario socket-push/pull ---\n");
        ws_direct_reset_for_tests();
        char sid[64] = {0};
        assert(ws_direct_init_session("src.pkg", PKG_SIZE, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(stream_server_session_start_ex(uri, SESSION) == 0);
        browser_arg_t ba = { E2E_SRC, PKG_SIZE,
                             (PKG_SIZE + SEG - 1) / SEG, 1, -1, NULL };
        pthread_t th;
        assert(pthread_create(&th, NULL, browser_thread, &ba) == 0);
        run_pull(1, PS5_SIM_DEFAULT_CHUNK, "socket");
        void *prc = NULL;
        /* pull returns after full delivery; push must have finished too */
        pthread_join(th, &prc);
        assert(ba.rc == 0);
        stream_server_session_stop();
        printf("  socket push/pull ok\n");
    }

    /* Scenario 2: producer aborts mid-bulk -> pull must fail fast. */
    {
        printf("--- scenario abort (fail fast, no hang) ---\n");
        ws_direct_reset_for_tests();
        char sid[64] = {0};
        assert(ws_direct_init_session("src.pkg", PKG_SIZE, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(stream_server_session_start_ex(uri, SESSION) == 0);
        push_arg_t pa = { E2E_SRC, PKG_SIZE, SEG, 500, (int)(PKG_SIZE / 2) };
        pthread_t th;
        assert(pthread_create(&th, NULL, push_thread, &pa) == 0);
        void *prc = NULL;
        pthread_join(th, &prc);
        ws_live_abort(); /* browser drop */
        run_pull(0, PS5_SIM_DEFAULT_CHUNK, "abort");
        stream_server_session_stop();
    }

    /* Scenario 3: minecraft jump with tiny slots (seeks, evictions,
     * resends) on a 100 MB file. */
    {
        printf("--- scenario minecraft-jump (100 MB, 8 slots) ---\n");
        ws_direct_reset_for_tests();
        setenv("WS_LIVE_SLOTS", "8", 1);
        ws_live_set_timeout_sec(120);
        const char *big = E2E_DIR "/big.pkg";
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cp %s %s && truncate -s 100M %s", E2E_SRC, big, big);
        assert(system(cmd) == 0);
        const uint64_t big_total = 100ULL * 1024 * 1024;
        char sid[64] = {0};
        assert(ws_direct_init_session("big.pkg", big_total, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(stream_server_session_start_ex(uri, SESSION) == 0);
        volatile int pull_done = 0;
        browser_arg_t ba = { big, big_total, big_total / SEG, 1, -1, &pull_done };
        pthread_t pth;
        assert(pthread_create(&pth, NULL, browser_thread, &ba) == 0);
        mc_arg_t ma = { big, big_total, -1 };
        pthread_t mth;
        assert(pthread_create(&mth, NULL, minecraft_pull_thread, &ma) == 0);
        void *bprc = NULL, *mprc = NULL;
        /* Join the puller first: the browser must keep serving resends
         * until the puller has drained (see linger in browser_thread). */
        pthread_join(mth, &mprc);
        pull_done = 1;
        pthread_join(pth, &bprc);
        printf("  push_rc=%d pull_rc=%d\n", ba.rc, ma.rc);
        assert(ba.rc == 0 && ma.rc == 0);
        stream_server_session_stop();
        unsetenv("WS_LIVE_SLOTS");
        ws_live_set_timeout_sec(30);
        printf("  minecraft-jump ok\n");
    }

    /* Scenario 3b: console-geometry parallel pivot. 90 segs / default 64
     * slots, two parallel phased readers:
     * tail bulk (56-74 / 74-89) then front pivot (0-18 / 18-36). The tail
     * bulk evicts the front; the pivot must be served through resends
     * while the socket stays responsive (no wedge, no idle kill). */
    {
        printf("--- scenario console-89mb-parallel-pivot (90 MB, 64 slots) ---\n");
        ws_direct_reset_for_tests();
        ws_live_set_timeout_sec(60);
        const char *big90 = E2E_DIR "/big90.pkg";
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "cp %s %s && truncate -s %llu %s",
                 E2E_SRC, big90, 90ULL * 1024 * 1024, big90);
        assert(system(cmd) == 0);
        const uint64_t big90_total = 90ULL * 1024 * 1024;
        char sid[64] = {0};
        assert(ws_direct_init_session("big90.pkg", big90_total, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(stream_server_session_start_ex(uri, SESSION) == 0);
        volatile int pull_done = 0;
        browser_arg_t ba = { big90, big90_total, big90_total / SEG, 1, -1, &pull_done };
        pthread_t pth;
        assert(pthread_create(&pth, NULL, browser_thread, &ba) == 0);
        pivot_arg_t pa1 = { big90, big90_total, -1, 0 };
        pivot_arg_t pa2 = { big90, big90_total, -1, 1 };
        pthread_t m1, m2;
        assert(pthread_create(&m1, NULL, pivot_reader_thread, &pa1) == 0);
        assert(pthread_create(&m2, NULL, pivot_reader_thread, &pa2) == 0);
        void *bprc = NULL, *r1 = NULL, *r2 = NULL;
        /* Join the pullers first: the browser must keep serving resends
         * until both have drained (see linger in browser_thread). */
        pthread_join(m1, &r1);
        pthread_join(m2, &r2);
        pull_done = 1;
        pthread_join(pth, &bprc);
        printf("  push_rc=%d pull1_rc=%d pull2_rc=%d\n", ba.rc, pa1.rc, pa2.rc);
        assert(ba.rc == 0 && pa1.rc == 0 && pa2.rc == 0);
        stream_server_session_stop();
        ws_live_set_timeout_sec(30);
        printf("  console-89mb-parallel-pivot ok\n");
    }

    /* Scenario 4: installer_start_live commits + mock worker completes. */
    {
        printf("--- scenario installer_start_live ---\n");
        ws_direct_reset_for_tests();
        assert(installer_init("http://127.0.0.1:8844/") == 0);
        char sid[64] = {0};
        assert(ws_direct_init_session("src.pkg", PKG_SIZE, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        push_arg_t pa = { E2E_SRC, PKG_SIZE, SEG, 1000, -1 };
        pthread_t th;
        assert(pthread_create(&th, NULL, push_thread, &pa) == 0);
        assert(installer_start_live(uri) == 0);
        installer_status_t st;
        int done = 0;
        for (int i = 0; i < 120; i++) {
            installer_get_status(&st);
            if (!st.is_installing) { done = 1; break; }
            usleep(250000);
        }
        void *prc = NULL;
        pthread_join(th, &prc);
        assert(prc == 0);
        installer_get_status(&st);
        printf("  install completed=%d failed=%d downloaded=%llu/%llu title='%s'\n",
               st.completed, st.failed,
               (unsigned long long)st.downloaded_bytes,
               (unsigned long long)st.total_bytes, st.title_id);
        assert(done && st.completed && !st.failed);
        assert(st.downloaded_bytes == PKG_SIZE && st.total_bytes == PKG_SIZE);
        assert(strcmp(st.title_id, "PPSA90012") == 0);
        installer_shutdown();
    }

    /* Scenario 5: unparseable header falls back instead of refusing. */
    {
        printf("--- scenario parse-fallback ---\n");
        ws_direct_reset_for_tests();
        assert(installer_init("http://127.0.0.1:8844/") == 0);
        const uint64_t bad_total = 2ULL * 1024 * 1024;
        char sid[64] = {0};
        assert(ws_direct_init_session("mystery.pkg", bad_total, sid, sizeof(sid)) == 0);
        char uri[128];
        snprintf(uri, sizeof(uri), "live:%s", sid);
        assert(system("rm -f " E2E_DIR "/zero.pkg"
                      " && dd if=/dev/zero of=" E2E_DIR "/zero.pkg bs=65536 count=32 2>/dev/null") == 0);
        push_arg_t pa = { E2E_DIR "/zero.pkg", bad_total, SEG, 1000, -1 };
        pthread_t th;
        assert(pthread_create(&th, NULL, push_thread, &pa) == 0);
        assert(installer_start_live(uri) == 0);
        installer_status_t st;
        int done = 0;
        for (int i = 0; i < 120; i++) {
            installer_get_status(&st);
            if (!st.is_installing) { done = 1; break; }
            usleep(250000);
        }
        void *prc = NULL;
        pthread_join(th, &prc);
        assert(prc == 0);
        installer_get_status(&st);
        printf("  fallback completed=%d failed=%d title='%s'\n",
               st.completed, st.failed, st.title_id);
        assert(done && st.completed && !st.failed);
        assert(strcmp(st.title_id, "UNKNOWN") == 0);
        installer_shutdown();
    }

    ws_direct_reset_for_tests();
    unsetenv("WS_DIRECT_PORT");
    printf("\n>>> DIRECT-INSTALL LIVE E2E PASSED! <<<\n");
    return 0;
}
