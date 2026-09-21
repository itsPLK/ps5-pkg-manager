/*
 * Host test: ws_live segment-cache session (NEW live-stream core).
 * Out-of-order writes, blocking reads with seek emission, eviction with
 * waiter shields, abort/timeout/finish semantics. Short timeouts so a
 * regression fails fast instead of hanging the suite.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "ws_stream.h"

#define SEG (WS_LIVE_SEG_SIZE)

static void fill_seg(unsigned char *b, uint64_t seg, unsigned char pat) {
    (void)seg;
    memset(b, pat, SEG);
}

static void reset(void) {
    ws_live_reset_for_tests();
    ws_live_set_timeout_sec(3);
    ws_live_set_seek_fn(NULL);
    ws_live_set_slots(WS_LIVE_SLOTS_DEFAULT);
}

/* Total 3 segments; tests use small files (ring_mb clamps to >=4 slots). */
static void test_basic_out_of_order(void) {
    reset();
    char sid[64] = {0};
    uint64_t total = 2 * SEG + 100;
    assert(ws_live_create("g.pkg", total, 0, sid, sizeof(sid)) == 0);
    assert(ws_live_session_active() && ws_live_check_id(sid));
    assert(ws_live_get_total() == total);

    static unsigned char seg0[WS_LIVE_SEG_SIZE];
    static unsigned char seg1[WS_LIVE_SEG_SIZE];
    static unsigned char seg2[100];
    fill_seg(seg0, 0, 0x11);
    fill_seg(seg1, 1, 0x22);
    memset(seg2, 0x33, sizeof(seg2));

    /* Out-of-order landing: seg 2, then 0, then 1. */
    assert(ws_live_write(2 * SEG, seg2, sizeof(seg2), NULL) == 0);
    assert(ws_live_write(0, seg0, SEG, NULL) == 0);
    assert(ws_live_write(SEG, seg1, SEG, NULL) == 0);

    /* Misaligned / oversize writes rejected. */
    unsigned char b[16] = {0};
    assert(ws_live_write(100, b, sizeof(b), NULL) != 0);
    assert(ws_live_write(0, b, sizeof(b), NULL) != 0); /* dup of present: partial */

    /* Header + full read back. */
    assert(ws_live_wait_header(5) == 0);
    uint8_t head[16];
    assert(ws_live_get_header(head, sizeof(head)) == sizeof(head));
    assert(head[0] == 0x11);
    unsigned char *all = malloc((size_t)total);
    assert(all);
    assert(ws_live_read(0, all, (size_t)total) == (long)total);
    assert(all[0] == 0x11 && all[SEG] == 0x22 && all[2 * SEG] == 0x33);
    free(all);

    /* Finish requires every segment present (they are). */
    assert(ws_live_finish() == 0);
    char st[1024];
    assert(ws_live_get_status(st, sizeof(st)) == 0);
    assert(strstr(st, "\"complete\":true") != NULL);
    printf("  basic-out-of-order ok\n");
}

static void test_finish_needs_all(void) {
    reset();
    char sid[64] = {0};
    assert(ws_live_create("f.pkg", 2 * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char s0[WS_LIVE_SEG_SIZE];
    memset(s0, 0xAA, sizeof(s0));
    assert(ws_live_write(0, s0, SEG, NULL) == 0);
    assert(ws_live_finish() != 0); /* seg 1 missing */
    static unsigned char s1[WS_LIVE_SEG_SIZE];
    memset(s1, 0xBB, sizeof(s1));
    assert(ws_live_write(SEG, s1, SEG, NULL) == 0);
    assert(ws_live_finish() == 0);
    printf("  finish-needs-all ok\n");
}

/* Seek-emission capture. */
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

static void test_seek_far_miss(void) {
    /* 100 segments; present seg 0 only; reader jumps to seg 90 (far beyond
     * the 8 MB grace) -> exactly one coalesced seek, then feed it. */
    reset();
    ws_live_set_seek_fn(capture_seek);
    g_nseeks = 0;
    char sid[64] = {0};
    uint64_t total = 100ULL * SEG;
    assert(ws_live_create("j.pkg", total, 0, sid, sizeof(sid)) == 0);
    static unsigned char s0[WS_LIVE_SEG_SIZE];
    memset(s0, 0x5A, sizeof(s0));
    assert(ws_live_write(0, s0, SEG, NULL) == 0);

    static unsigned char rbuf[65536];
    read_job_t j = { .off = 90ULL * SEG, .len = sizeof(rbuf), .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    /* Wait for the coalesced seek (reader blocks ~immediately). */
    int got = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        pthread_mutex_lock(&g_sq_mu);
        got = g_nseeks;
        pthread_mutex_unlock(&g_sq_mu);
        if (got > 0) break;
    }
    assert(got > 0 && g_seeks[0] == 90);
    /* Feed the requested segment; the read must complete. */
    static unsigned char s90[WS_LIVE_SEG_SIZE];
    memset(s90, 0x9E, sizeof(s90));
    assert(ws_live_write(90ULL * SEG, s90, SEG, NULL) == 0);
    pthread_join(th, NULL);
    assert(j.rc == (long)sizeof(rbuf));
    for (size_t i = 0; i < sizeof(rbuf); i++) assert(rbuf[i] == 0x9E);
    printf("  seek-far-miss ok (seek seg=%llu)\n", (unsigned long long)g_seeks[0]);
    ws_live_set_seek_fn(NULL);
}

static void test_near_miss_seeks(void) {
    /* Sequential lag also seeks (coalesced): with a redirectable baseline
     * no miss can be assumed to arrive on its own. */
    reset();
    ws_live_set_seek_fn(capture_seek);
    g_nseeks = 0;
    char sid[64] = {0};
    assert(ws_live_create("n.pkg", 4ULL * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char s0[WS_LIVE_SEG_SIZE];
    memset(s0, 0x11, sizeof(s0));
    assert(ws_live_write(0, s0, SEG, NULL) == 0);
    static unsigned char rbuf[65536];
    /* Read straddling present seg 0 into missing seg 1. */
    read_job_t j = { .off = SEG - 100, .len = 200, .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    usleep(300000);
    pthread_mutex_lock(&g_sq_mu);
    int got = g_nseeks;
    uint64_t first = got > 0 ? g_seeks[0] : 9999;
    pthread_mutex_unlock(&g_sq_mu);
    assert(got > 0 && first == 1);
    static unsigned char s1[WS_LIVE_SEG_SIZE];
    memset(s1, 0x22, sizeof(s1));
    assert(ws_live_write(SEG, s1, SEG, NULL) == 0);
    pthread_join(th, NULL);
    assert(j.rc == 200);
    printf("  near-miss-seeks ok\n");
    ws_live_set_seek_fn(NULL);
}

static void test_eviction_and_reseek(void) {
    /* 4 slots: only CONSUMED segments may be evicted. Write 0..3 (full),
     * consume 1+2, write 4 (evicts LRU-consumed seg 1, never pinned seg 0
     * or unconsumed 2..3). A re-read of evicted seg 1 misses, seeks, and
     * succeeds once re-fed. */
    reset();
    ws_live_set_slots(4);
    ws_live_set_seek_fn(capture_seek);
    g_nseeks = 0;
    char sid[64] = {0};
    assert(ws_live_create("e.pkg", 8ULL * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char w[WS_LIVE_SEG_SIZE];
    for (uint64_t s = 0; s < 4; s++) {
        memset(w, (int)(0xA0 + s), sizeof(w));
        assert(ws_live_write(s * SEG, w, SEG, NULL) == 0);
    }
    /* Consume segs 1 and 2. */
    static unsigned char r12[16];
    assert(ws_live_read(SEG, r12, sizeof(r12)) == (long)sizeof(r12));
    assert(r12[0] == 0xA1);
    assert(ws_live_read(2ULL * SEG, r12, sizeof(r12)) == (long)sizeof(r12));
    assert(r12[0] == 0xA2);
    /* Seg 4 forces eviction of LRU-consumed seg 1. */
    memset(w, 0xA4, sizeof(w));
    assert(ws_live_write(4ULL * SEG, w, SEG, NULL) == 0);
    /* Pinned seg 0 and unconsumed seg 3 survive. */
    static unsigned char r0[16];
    assert(ws_live_read(0, r0, sizeof(r0)) == (long)sizeof(r0));
    assert(r0[0] == 0xA0);
    static unsigned char r3[16];
    assert(ws_live_read(3ULL * SEG, r3, sizeof(r3)) == (long)sizeof(r3));
    assert(r3[0] == 0xA3);
    /* Evicted seg 1: read misses, seeks, then succeeds once re-fed. */
    static unsigned char r1[16];
    read_job_t j = { .off = SEG, .len = sizeof(r1), .rc = -99, .buf = r1 };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    usleep(300000);
    pthread_mutex_lock(&g_sq_mu);
    int got = g_nseeks;
    pthread_mutex_unlock(&g_sq_mu);
    assert(got > 0);
    memset(w, 0xA1, sizeof(w));
    assert(ws_live_write(SEG, w, SEG, NULL) == 0);
    pthread_join(th, NULL);
    assert(j.rc == (long)sizeof(r1) && r1[0] == 0xA1);
    printf("  eviction-and-reseek ok\n");
    ws_live_set_seek_fn(NULL);
    ws_live_set_slots(WS_LIVE_SLOTS_DEFAULT);
}

typedef struct {
    uint64_t off;
    size_t len;
    int rc;
} write_job_t;

static void *write_thread(void *arg) {
    write_job_t *j = (write_job_t *)arg;
    static unsigned char w[WS_LIVE_SEG_SIZE];
    memset(w, 0x5A, sizeof(w));
    uint64_t off = j->off;
    size_t left = j->len;
    while (left > 0) {
        size_t n = left > SEG ? SEG : left;
        int rc = ws_live_write(off, w, n, NULL);
        if (rc != 0) { j->rc = rc; return NULL; }
        off += n;
        left -= n;
    }
    j->rc = 0;
    return NULL;
}

static void test_writer_blocks(void) {
    /* 4 slots, no readers: the 5th segment write must block (not drop),
     * then complete once a read frees a consumed slot. */
    reset();
    ws_live_set_slots(4);
    char sid[64] = {0};
    assert(ws_live_create("wb.pkg", 8ULL * SEG, 0, sid, sizeof(sid)) == 0);
    write_job_t j = { .off = 0, .len = 5ULL * SEG, .rc = -99 };
    pthread_t th;
    assert(pthread_create(&th, NULL, write_thread, &j) == 0);
    usleep(500000); /* writer fills 4 slots, blocks on the 5th */
    static unsigned char rbuf[WS_LIVE_SEG_SIZE];
    assert(ws_live_read(SEG, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf));
    assert(rbuf[0] == 0x5A);
    pthread_join(th, NULL);
    assert(j.rc == 0);
    printf("  writer-blocks ok\n");
    ws_live_set_slots(WS_LIVE_SLOTS_DEFAULT);
}

static void test_abort_unblocks(void) {
    reset();
    char sid[64] = {0};
    assert(ws_live_create("a.pkg", 100ULL * SEG, 0, sid, sizeof(sid)) == 0);
    static unsigned char rbuf[64];
    read_job_t j = { .off = 50ULL * SEG, .len = sizeof(rbuf), .rc = -99, .buf = rbuf };
    pthread_t th;
    assert(pthread_create(&th, NULL, read_thread, &j) == 0);
    usleep(200000);
    ws_live_abort();
    pthread_join(th, NULL);
    assert(j.rc == -1);
    unsigned char w[8] = {0};
    assert(ws_live_write(0, w, sizeof(w), NULL) != 0);
    printf("  abort-unblocks ok\n");
}

static void test_read_timeout(void) {
    reset();
    char sid[64] = {0};
    assert(ws_live_create("t.pkg", 100ULL * SEG, 0, sid, sizeof(sid)) == 0);
    unsigned char rbuf[64];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    assert(ws_live_read(50ULL * SEG, rbuf, sizeof(rbuf)) == -1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long dt = (long)(t1.tv_sec - t0.tv_sec);
    assert(dt >= 3 && dt < 15);
    printf("  read-timeout ok (%lds)\n", dt);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("==============================================\n");
    printf(">>> RUNNING WS_LIVE SEGMENT TEST <<<\n");
    printf("==============================================\n");
    srand(999);
    test_basic_out_of_order();
    test_finish_needs_all();
    test_seek_far_miss();
    test_near_miss_seeks();
    test_eviction_and_reseek();
    test_writer_blocks();
    test_abort_unblocks();
    test_read_timeout();
    ws_live_reset_for_tests();
    printf("\n>>> ALL WS_LIVE TESTS PASSED! <<<\n");
    return 0;
}
