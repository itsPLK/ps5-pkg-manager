/*
 * Host test: far-seek eviction in the ws_live segment cache (NEW).
 *
 * Exercises the production deadlock path that the existing suite does not:
 * the ring is full of UNREAD front (no segment has been served/passed), the
 * reader has jumped far ahead to a target segment, and the writer then lands
 * that far segment. A near-write rule (served-past only) would block forever;
 * a far-seek write (seg >= peak_seg) may reclaim an unread front slot.
 *
 * Short timeouts so a regression fails fast instead of hanging the suite.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "ws_stream.h"

#define SEG (WS_LIVE_SEG_SIZE)

static void reset(void) {
    ws_live_reset_for_tests();
    ws_live_set_timeout_sec(4);
    ws_live_set_seek_fn(NULL);
}

static uint64_t g_seeks[16];
static int g_nseeks = 0;
static pthread_mutex_t g_sq_mu = PTHREAD_MUTEX_INITIALIZER;

static void capture_seek(uint64_t seg) {
    pthread_mutex_lock(&g_sq_mu);
    if (g_nseeks < 16) g_seeks[g_nseeks++] = seg;
    pthread_mutex_unlock(&g_sq_mu);
}

typedef struct {
    uint64_t off;
    size_t len;
    long rc;
    unsigned char *buf;
} read_job_t;

static void *read_thread(void *arg) {
    read_job_t *j = (read_job_t *)arg;
    j->rc = ws_live_read(j->off, j->buf, j->len);
    return NULL;
}

/* Wait (bounded) until the reader emitted a seek for want_seg. */
static void wait_seek(uint64_t want_seg) {
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        pthread_mutex_lock(&g_sq_mu);
        int found = 0;
        for (int k = 0; k < g_nseeks; k++) {
            if (g_seeks[k] == want_seg) { found = 1; break; }
        }
        pthread_mutex_unlock(&g_sq_mu);
        if (found) return;
    }
    assert(0 && "seek never emitted");
}

/* Ring full of unread front; a far read then a far write that must reclaim
 * an unread front slot instead of blocking until timeout. */
static void test_far_write_reclaims_unread_front(void) {
    reset();
    ws_live_set_slots(4);
    ws_live_set_seek_fn(capture_seek);
    g_nseeks = 0;
    char sid[64] = {0};
    uint64_t total = 8ULL * SEG;
    assert(ws_live_create("fe.pkg", total, 0, sid, sizeof(sid)) == 0);

    /* Fill the ring with the first four segments; none has been read, so
     * nothing is served-past. peak_seg is still 0. */
    static unsigned char w[WS_LIVE_SEG_SIZE];
    for (uint64_t s = 0; s < 4; s++) {
        memset(w, (int)(0xC0 + s), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }

    /* Reader jumps far ahead to seg 6: this seeks (marks peak_seg = 6) and
     * blocks until seg 6 arrives. */
    static unsigned char rbuf[65536];
    read_job_t j = { .off = 6ULL * SEG, .len = sizeof(rbuf), .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);

    int got = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        pthread_mutex_lock(&g_sq_mu);
        got = g_nseeks;
        pthread_mutex_unlock(&g_sq_mu);
        if (got > 0) break;
    }
    assert(got > 0 && g_seeks[0] == 6);

    /* The far write lands. Because seg 6 >= peak_seg (6), it may reclaim an
     * unread front slot (peak-past) — this must NOT block/timeout. */
    memset(w, 0xFE, sizeof(w));
    assert(ws_live_write(6ULL * SEG, w, SEG, NULL) == 0);

    pthread_join(th, NULL);
    assert(j.rc == (long)sizeof(rbuf));
    for (size_t i = 0; i < sizeof(rbuf); i++) assert(rbuf[i] == 0xFE);
    printf("  far-write-reclaims-unread-front ok (seek seg=%llu)\n",
           (unsigned long long)g_seeks[0]);
}

/* Regression for the >64 MiB deadlock: a gap/baseline write advances after a
 * far seek.
 *
 * The reader jumps far ahead (peak_seg high) and blocks on the target, while
 * the ring fills with the skipped baseline prefix -- none of it served-past,
 * because the reader never reads those segments contiguously. A near write to
 * one of those skipped segments used to block until timeout (near writes may
 * only reclaim served-past slots, and run_hi never advanced). With gap
 * eviction a skipped segment that has already been acknowledged is reclaimable,
 * so the write succeeds and the file keeps cycling through the ring instead of
 * stalling at the ring boundary.
 */
static void test_gap_write_advances_after_far_seek(void) {
    reset();
    ws_live_set_slots(4);
    ws_live_set_seek_fn(capture_seek);
    g_nseeks = 0;
    char sid[64] = {0};
    uint64_t total = 100ULL * SEG;
    assert(ws_live_create("gap.pkg", total, 0, sid, sizeof(sid)) == 0);

    /* Fill the ring with the first four segments: none read, so none is
     * served-past, but all four have been acknowledged (acked_ever). */
    static unsigned char w[WS_LIVE_SEG_SIZE];
    for (uint64_t s = 0; s < 4; s++) {
        memset(w, (int)(0xC0 + s), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }

    /* Reader jumps far ahead to seg 90: sets peak_seg = 90 and blocks on it. */
    static unsigned char rbuf[65536];
    read_job_t j = { .off = 90ULL * SEG, .len = sizeof(rbuf), .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    int got = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        pthread_mutex_lock(&g_sq_mu);
        got = g_nseeks;
        pthread_mutex_unlock(&g_sq_mu);
        if (got > 0) break;
    }
    assert(got > 0 && g_seeks[0] == 90);

    /* Gap write to seg 64 (64 < peak_seg 90, not served-past): under the old
     * near-write rule this blocked until timeout; gap eviction reclaims an
     * already-acked, unread front slot, so it succeeds here. */
    memset(w, 0xAA, sizeof(w));
    assert(ws_live_write(64ULL * SEG, w, SEG, NULL) == 0);

    /* Feed the far target: the blocked read must then complete whole. */
    memset(w, 0xEE, sizeof(w));
    assert(ws_live_write(90ULL * SEG, w, SEG, NULL) == 0);

    pthread_join(th, NULL);
    assert(j.rc == (long)sizeof(rbuf));
    for (size_t i = 0; i < sizeof(rbuf); i++) assert(rbuf[i] == 0xEE);
    printf("  gap-write-advances-after-far-seek ok (seek seg=%llu)\n",
           (unsigned long long)g_seeks[0]);
}

/* Console-geometry front pivot (WS.md §5): 90 segs / 64 slots. Pre-fill
 * 0..63, stream the tail 64..89 through (evicting the front exactly like
 * the production tail bulk), then a parked front read must be serveable
 * at once. Under far-only eviction the waited write for seg 1 wedged the
 * conn worker; here it must land via Rule B, and the unwaited gap write
 * for seg 18 via Rule C. */
static void test_front_pivot_waited_writes_succeed(void) {
    reset();
    ws_live_set_slots(64);
    ws_live_set_seek_fn(capture_seek);
    pthread_mutex_lock(&g_sq_mu);
    g_nseeks = 0;
    pthread_mutex_unlock(&g_sq_mu);
    char sid[64] = {0};
    assert(ws_live_create("pivot.pkg", 90ULL * SEG, 0, sid, sizeof(sid)) == 0);

    /* Pre-fill the 64-slot ring exactly (segs 0..63). */
    static unsigned char w[WS_LIVE_SEG_SIZE];
    for (uint64_t s = 0; s < 64; s++) {
        memset(w, (int)(0xA0 + (s & 0x0F)), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }

    /* Tail bulk: park a reader on seg 89 (peak_seg = 89), then stream
     * 64..89 through. Each far write reclaims the oldest front slot, so
     * segs 1..26 are evicted in order -- the production pattern. */
    static unsigned char rtail[65536];
    read_job_t tj = { .off = 89ULL * SEG, .len = sizeof(rtail), .rc = -99, .buf = rtail };
    pthread_t tth;
    assert(pthread_create(&tth, NULL, read_thread, &tj) == 0);
    wait_seek(89);
    for (uint64_t s = 64; s < 90; s++) {
        memset(w, (int)(0xB0 + (s & 0x0F)), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }
    pthread_join(tth, NULL);
    assert(tj.rc == (long)sizeof(rtail));

    /* Front pivot: park a reader on evicted seg 1; the waited write must
     * land immediately (old code: blocked until the 4 s timeout). */
    static unsigned char r1[65536];
    read_job_t j1 = { .off = 1ULL * SEG, .len = sizeof(r1), .rc = -99, .buf = r1 };
    pthread_t th1;
    pthread_mutex_lock(&g_sq_mu);
    g_nseeks = 0;
    pthread_mutex_unlock(&g_sq_mu);
    assert(pthread_create(&th1, NULL, read_thread, &j1) == 0);
    wait_seek(1);
    memset(w, 0xBB, sizeof(w));
    assert(ws_live_write(1ULL * SEG, w, SEG, NULL) == 0);
    pthread_join(th1, NULL);
    assert(j1.rc == (long)sizeof(r1));
    for (size_t i = 0; i < sizeof(r1); i++) assert(r1[i] == 0xBB);

    /* Unwaited gap write (seg 18) must also land via demand paging. */
    memset(w, 0xCC, sizeof(w));
    assert(ws_live_write(18ULL * SEG, w, SEG, NULL) == 0);
    static unsigned char r18[16];
    assert(ws_live_read(18ULL * SEG, r18, sizeof(r18)) == (long)sizeof(r18));
    for (size_t i = 0; i < sizeof(r18); i++) assert(r18[i] == 0xCC);
    printf("  front-pivot-waited-writes ok\n");
}

/* Non-blocking store (§9.1): a full sequential ring (peak_seg == 0, no
 * served slots) reports busy instantly instead of wedging, and bad
 * offsets are still fatal. */
static void test_try_write_busy_never_blocks(void) {
    reset();
    ws_live_set_slots(4);
    char sid[64] = {0};
    assert(ws_live_create("busy.pkg", 8ULL * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char w[WS_LIVE_SEG_SIZE];
    for (uint64_t s = 0; s < 4; s++) {
        memset(w, (int)(0xD0 + s), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }
    memset(w, 0xFF, sizeof(w));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(ws_live_try_write(4ULL * SEG, w, SEG) == -2); /* busy, instant */
    clock_gettime(CLOCK_MONOTONIC, &t1);
    assert(t1.tv_sec == t0.tv_sec); /* no waiting happened */
    assert(ws_live_try_write(100ULL * SEG, w, SEG) == -1); /* bad offset */
    /* Blocking write still completes once a read frees a served slot. */
    static unsigned char rbuf[WS_LIVE_SEG_SIZE];
    assert(ws_live_read(SEG, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf));
    assert(ws_live_write(4ULL * SEG, w, SEG, NULL) == 0);
    printf("  try-write-busy-never-blocks ok\n");
}

/* Parked-reader census for the anti-idle guard (§9.4). */
static void test_waiter_count_tracks_parked_readers(void) {
    reset();
    char sid[64] = {0};
    assert(ws_live_create("wc.pkg", 8ULL * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char w[WS_LIVE_SEG_SIZE];
    memset(w, 0x11, sizeof(w));
    assert(ws_live_write(0, w, SEG, NULL) == 0);
    assert(ws_live_waiter_count() == 0);
    static unsigned char rbuf[65536];
    read_job_t j = { .off = 5ULL * SEG, .len = sizeof(rbuf), .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    int seen = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        if (ws_live_waiter_count() == 1) { seen = 1; break; }
    }
    assert(seen);
    ws_live_abort();
    pthread_join(th, NULL);
    assert(j.rc == -1);
    assert(ws_live_waiter_count() == 0);
    printf("  waiter-count-tracks-parked-readers ok\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("==============================================\n");
    printf(">>> RUNNING WS_LIVE FAR-SEEK TEST <<<\n");
    printf("==============================================\n");
    test_far_write_reclaims_unread_front();
    test_gap_write_advances_after_far_seek();
    test_front_pivot_waited_writes_succeed();
    test_try_write_busy_never_blocks();
    test_waiter_count_tracks_parked_readers();
    ws_live_reset_for_tests();
    printf("\n>>> ALL WS_LIVE FAR-SEEK TESTS PASSED! <<<\n");
    return 0;
}
