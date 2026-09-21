/*
 * PKG Manager - live streaming session (demand-paged segment cache).
 * See include/ws_stream.h for the contract.
 */

#include "ws_stream.h"
#include "installer.h" /* install_log only */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#define SEG (WS_LIVE_SEG_SIZE)

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t changed; /* any arrival/finish/abort wakes waiters */
    int inited;
    int active;
    int upload_complete;
    int aborted;
    int refs;
    int free_pending;
    char session_id[64];
    char filename[256];
    uint64_t total;
    uint64_t nsegs;
    uint8_t *present;   /* per-segment present flags */
    int *slot_of;       /* per-segment RAM slot index, -1 = absent */
    uint8_t *slots;     /* slots * SEG bytes */
    int nslots;
    uint64_t *last_use; /* per-slot LRU stamp */
    int *slot_seg;      /* per-slot file segment, -1 = free */
    int *waiters;       /* per-segment waiter count (eviction shield) */
    uint8_t *acked_ever;/* per-segment stored-at-least-once (finish gate) */
    uint64_t *seek_sent;/* per-segment last seek-emission stamp (ms) */
    uint64_t present_count;
    uint64_t served;    /* bytes delivered to readers (status only) */
    uint64_t run_hi;    /* offset past reader's last served read (eviction HWM) */
    uint64_t peak_seg;  /* highest segment the reader has sought (far-seek HWM) */
    uint64_t tick;
    ws_live_seek_fn seek_fn;
} live_t;

static live_t g_lv;
static int g_timeout_sec = WS_LIVE_TIMEOUT_DEFAULT_SEC;
static int g_slots_cfg = WS_LIVE_SLOTS_DEFAULT;

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static void json_filename(const char *src, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            out[n++] = '\\';
            out[n++] = (char)*p;
        } else if (*p < 0x20) {
            snprintf(out + n, cap - n, "\\u%04x", *p);
            n += 6;
        } else {
            out[n++] = (char)*p;
        }
    }
    out[n] = '\0';
}

static void live_lock_init(void) {
    if (!g_lv.inited) {
        pthread_mutex_init(&g_lv.mu, NULL);
        pthread_cond_init(&g_lv.changed, NULL);
        g_lv.inited = 1;
    }
}

int ws_live_set_timeout_sec(int sec) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    g_timeout_sec = (sec > 0) ? sec : WS_LIVE_TIMEOUT_DEFAULT_SEC;
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

int ws_live_set_slots(int slots) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (slots >= 4 && slots <= 4096) g_slots_cfg = slots;
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

void ws_live_set_seek_fn(ws_live_seek_fn fn) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    g_lv.seek_fn = fn;
    pthread_mutex_unlock(&g_lv.mu);
}

/* 1 s tick waits against an absolute deadline; returns 1 when expired. */
static int tick_wait(pthread_cond_t *cond, pthread_mutex_t *mu,
                     const struct timespec *deadline) {
    struct timespec tick;
    clock_gettime(CLOCK_REALTIME, &tick);
    tick.tv_sec += 1;
    if (tick.tv_sec > deadline->tv_sec ||
        (tick.tv_sec == deadline->tv_sec && tick.tv_nsec > deadline->tv_nsec)) {
        tick = *deadline;
    }
    pthread_cond_timedwait(cond, mu, &tick);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (now.tv_sec > deadline->tv_sec ||
            (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec));
}

static int default_slots(void) {
    const char *env = getenv("WS_LIVE_SLOTS");
    if (env && env[0]) {
        long v = strtol(env, NULL, 10);
        if (v >= 4 && v <= 4096) return (int)v;
    }
    return g_slots_cfg;
}

static void free_locked(void) {
    free(g_lv.present);
    free(g_lv.slot_of);
    free(g_lv.slots);
    free(g_lv.last_use);
    free(g_lv.slot_seg);
    free(g_lv.waiters);
    free(g_lv.acked_ever);
    free(g_lv.seek_sent);
    g_lv.present = NULL;
    g_lv.slot_of = NULL;
    g_lv.slots = NULL;
    g_lv.last_use = NULL;
    g_lv.slot_seg = NULL;
    g_lv.waiters = NULL;
    g_lv.acked_ever = NULL;
    g_lv.seek_sent = NULL;
    g_lv.free_pending = 0;
}

int ws_live_create(const char *filename, uint64_t total_size, int ring_mb,
                   char *out_session_id, size_t sid_max) {
    if (!filename || !filename[0] || total_size == 0) return -1;
    const char *slash = strrchr(filename, '/');
    const char *base = slash ? slash + 1 : filename;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return -1;

    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (g_lv.active) {
        if (!g_lv.aborted && strcmp(g_lv.filename, base) == 0 &&
            g_lv.total == total_size) {
            if (out_session_id && sid_max)
                snprintf(out_session_id, sid_max, "%s", g_lv.session_id);
            pthread_mutex_unlock(&g_lv.mu);
            return 0;
        }
        pthread_mutex_unlock(&g_lv.mu);
        return -2;
    }
    if (g_lv.refs > 0) {
        pthread_mutex_unlock(&g_lv.mu);
        return -2;
    }
    uint64_t nsegs = (total_size + SEG - 1) / SEG;
    int nslots;
    if (ring_mb > 0) {
        nslots = ring_mb; /* 1 MB segments: budget == slots */
        if (nslots < 4) nslots = 4;
        if (nslots > 4096) nslots = 4096;
    } else {
        nslots = default_slots();
    }
    if ((uint64_t)nslots > nsegs + 4) nslots = (int)(nsegs + 4);
    if (nslots < 4) nslots = 4;
    uint8_t *present = calloc(nsegs ? (size_t)nsegs : 1, 1);
    int *slot_of = malloc((nsegs ? (size_t)nsegs : 1) * sizeof(int));
    uint8_t *slots = malloc((size_t)nslots * SEG);
    uint64_t *last_use = calloc((size_t)nslots, sizeof(uint64_t));
    int *slot_seg = malloc((size_t)nslots * sizeof(int));
    int *waiters = calloc(nsegs ? (size_t)nsegs : 1, sizeof(int));
    uint8_t *acked_ever = calloc(nsegs ? (size_t)nsegs : 1, 1);
    uint64_t *seek_sent = calloc(nsegs ? (size_t)nsegs : 1, sizeof(uint64_t));
    if (!present || !slot_of || !slots || !last_use || !slot_seg ||
        !waiters || !acked_ever || !seek_sent) {
        free(present); free(slot_of); free(slots); free(last_use);
        free(slot_seg); free(waiters); free(acked_ever);
        free(seek_sent);
        pthread_mutex_unlock(&g_lv.mu);
        return -1;
    }
    for (uint64_t i = 0; i < nsegs; i++) slot_of[i] = -1;
    for (int i = 0; i < nslots; i++) slot_seg[i] = -1;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    static unsigned long ctr = 0;
    ctr++;
    snprintf(g_lv.session_id, sizeof(g_lv.session_id), "%08lx%08lx",
             (unsigned long)tv.tv_sec ^ (unsigned long)getpid() ^ (ctr * 2654435761UL),
             (unsigned long)tv.tv_usec ^ (unsigned long)rand() ^ (ctr * 40503UL));
    snprintf(g_lv.filename, sizeof(g_lv.filename), "%s", base);
    g_lv.total = total_size;
    g_lv.nsegs = nsegs;
    g_lv.present = present;
    g_lv.slot_of = slot_of;
    g_lv.slots = slots;
    g_lv.nslots = nslots;
    g_lv.last_use = last_use;
    g_lv.slot_seg = slot_seg;
    g_lv.waiters = waiters;
    g_lv.acked_ever = acked_ever;
    g_lv.seek_sent = seek_sent;
    g_lv.present_count = 0;
    g_lv.served = 0;
    g_lv.run_hi = 0;
    g_lv.peak_seg = 0;
    g_lv.upload_complete = 0;
    g_lv.aborted = 0;
    g_lv.refs = 0;
    g_lv.free_pending = 0;
    g_lv.active = 1;
    if (out_session_id && sid_max)
        snprintf(out_session_id, sid_max, "%s", g_lv.session_id);
    install_log("[WS] live session '%s' ready: file='%s' total=%llu segs=%llu slots=%d",
                g_lv.session_id, base, (unsigned long long)total_size,
                (unsigned long long)nsegs, nslots);
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

int ws_live_session_active(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    int a = g_lv.active;
    pthread_mutex_unlock(&g_lv.mu);
    return a;
}

int ws_live_check_id(const char *session_id) {
    if (!session_id || !session_id[0]) return 0;
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    int ok = g_lv.active && strcmp(g_lv.session_id, session_id) == 0;
    pthread_mutex_unlock(&g_lv.mu);
    return ok;
}

static uint64_t seg_len_locked(uint64_t seg) {
    uint64_t start = seg * SEG;
    if (start >= g_lv.total) return 0;
    uint64_t left = g_lv.total - start;
    return (left < SEG) ? left : SEG;
}

/* Make room for file segment seg; caller holds the lock. Returns the RAM
 * slot index, or -1 when nothing may be evicted right now. Seg 0 is pinned
 * and any segment a reader is blocked on is shielded; every other present
 * segment is evictable when ANY of these hold (see ws_stream.h):
 *
 *   A) served-past: the reader has been delivered past it
 *      (run_hi >= its end) -- reclaimable for any write. This is the only
 *      rule active during sequential playback (peak_seg == 0 and nothing
 *      waited-on), so the frozen lockstep contract is untouched.
 *   B) the incoming write is urgently waited-on by a parked reader -- it
 *      may reclaim any acknowledged, unshielded slot so the blocked read
 *      unblocks at once (front-pivot resends land here).
 *   C) non-sequential mode (peak_seg > 0: the reader has sought ahead):
 *      any acknowledged slot is reclaimable, LRU. A far write
 *      (seg >= peak_seg) takes a slot from the region it jumped over; any
 *      other (gap/baseline) write cycles through acknowledged slots so a
 *      phase pivot (tail bulk, then front bulk) keeps flowing instead of
 *      wedging the single conn worker.
 *
 * The caller either waits and retries (ws_live_write) or reports busy
 * (ws_live_try_write); bytes are never dropped. */
static int alloc_slot_locked(uint64_t seg) {
    if (seg < g_lv.nsegs && g_lv.slot_of[seg] >= 0)
        return g_lv.slot_of[seg]; /* Already resident */
    int free_slot = -1;
    for (int i = 0; i < g_lv.nslots; i++) {
        if (g_lv.slot_seg[i] < 0) { free_slot = i; break; }
    }
    if (free_slot < 0) {
        int incoming_waited = (seg < g_lv.nsegs && g_lv.waiters[seg] > 0);
        int far = (seg >= g_lv.peak_seg);
        uint64_t oldest = 0;
        int oldest_seg = -1;
        int first = 1;
        for (uint64_t s = 1; s < g_lv.nsegs; s++) {
            if (!g_lv.present[s] || g_lv.slot_of[s] < 0) continue;
            if (g_lv.waiters[s] > 0) continue;
            int evictable = 0;
            if (g_lv.run_hi >= (s + 1) * SEG) {
                evictable = 1;                            /* A: served-past */
            } else if (incoming_waited && g_lv.acked_ever[s]) {
                evictable = 1;                            /* B: urgent read */
            } else if (g_lv.peak_seg > 0 && g_lv.acked_ever[s] &&
                       (far || s != seg)) {
                evictable = 1;                            /* C: demand page */
            }
            if (!evictable) continue;
            uint64_t stamp = g_lv.last_use[g_lv.slot_of[s]];
            if (first || stamp < oldest) {
                oldest = stamp;
                oldest_seg = (int)s;
                first = 0;
            }
        }
        if (oldest_seg < 0) return -1;
        free_slot = g_lv.slot_of[oldest_seg];
        g_lv.present[oldest_seg] = 0;
        g_lv.slot_of[oldest_seg] = -1;
    }
    if (seg < g_lv.nsegs) {
        g_lv.slot_of[seg] = free_slot;
        g_lv.slot_seg[free_slot] = (int)seg;
    }
    return free_slot;
}

/* Validate and store one whole segment; caller holds the lock. data/len/
 * offset describe the caller's whole message (for slice math).
 * Returns the slot index on success, 1 when the segment does not overlap
 * the message (nothing to do), -1 on protocol error, -2 when no slot is
 * evictable right now (busy). Single validation/store truth shared by
 * the blocking and non-blocking writers. */
static int store_seg_locked(uint64_t seg, const void *data,
                            uint64_t offset, size_t len) {
    uint64_t s_start = seg * SEG;
    uint64_t s_len = seg_len_locked(seg);
    uint64_t last_end = offset + len;
    uint64_t w_start = (offset > s_start) ? offset : s_start;
    uint64_t w_end = (last_end < s_start + s_len) ? last_end : s_start + s_len;
    if (w_end <= w_start) return 1;
    if (w_end - w_start != s_len) {
        /* Partial segment write: only the short final segment may
         * arrive piecemeal; anything else is a protocol error. */
        return -1;
    }
    int slot = alloc_slot_locked(seg);
    if (slot < 0) return -2;
    memcpy(g_lv.slots + (size_t)slot * SEG,
           (const uint8_t *)data + (w_start - offset), (size_t)s_len);
    if (!g_lv.present[seg]) {
        g_lv.present[seg] = 1;
        g_lv.present_count++;
    }
    g_lv.acked_ever[seg] = 1;
    g_lv.last_use[slot] = ++g_lv.tick;
    return slot;
}

int ws_live_write(uint64_t offset, const void *data, size_t len,
                  uint64_t *out_expected) {
    if (out_expected) *out_expected = 0;
    if (!data && len > 0) return -1;
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active || g_lv.aborted) { pthread_mutex_unlock(&g_lv.mu); return -1; }
    if (len == 0) { pthread_mutex_unlock(&g_lv.mu); return 0; }
    if (offset + len > g_lv.total || offset % SEG != 0) {
        pthread_mutex_unlock(&g_lv.mu);
        return -1;
    }
    /* Whole 1 MB segments, or the short final segment. */
    uint64_t first_seg = offset / SEG;
    uint64_t last_end = offset + len;
    uint64_t last_seg = (last_end - 1) / SEG;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += g_timeout_sec;
    for (uint64_t s = first_seg; s <= last_seg; s++) {
        /* No evictable slot (window full of unread data): wait for readers
         * instead of dropping bytes. Correct backpressure; abort/timeout
         * bound the wait. A parked write logs its state every 2 s so the
         * next production capture names the wedged segment directly
         * instead of showing only re-seeks. */
        uint64_t wstart = now_ms();
        unsigned wmark = 0;
        for (;;) {
            if (g_lv.aborted) { pthread_mutex_unlock(&g_lv.mu); return -1; }
            int r = store_seg_locked(s, data, offset, len);
            if (r == 1) break; /* no overlap: nothing to do */
            if (r == -1) { pthread_mutex_unlock(&g_lv.mu); return -1; }
            if (r >= 0) break; /* stored */
            if (tick_wait(&g_lv.changed, &g_lv.mu, &dl)) {
                install_log("[WS] live write timed out waiting for readers");
                pthread_mutex_unlock(&g_lv.mu);
                return -1;
            }
            {
                unsigned wsecs = (unsigned)((now_ms() - wstart) / 1000ULL);
                if (wsecs >= 2 && wsecs / 2 > wmark) {
                    uint64_t wtot = 0;
                    wmark = wsecs / 2;
                    for (uint64_t q = 0; q < g_lv.nsegs; q++)
                        wtot += (uint64_t)g_lv.waiters[q];
                    install_log("[WS] WARNING: live write seg %llu blocked %us "
                                "(peak=%llu run_hi=%llu wait_self=%d wait_tot=%llu "
                                "present=%llu/%llu)",
                                (unsigned long long)s, wsecs,
                                (unsigned long long)g_lv.peak_seg,
                                (unsigned long long)g_lv.run_hi,
                                (s < g_lv.nsegs) ? g_lv.waiters[s] : 0,
                                (unsigned long long)wtot,
                                (unsigned long long)g_lv.present_count,
                                (unsigned long long)g_lv.nsegs);
                }
            }
        }
    }
    pthread_cond_broadcast(&g_lv.changed);
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

/* Non-blocking store for the WS conn worker: a single allocation attempt,
 * never waits. Returns 0 stored, -1 fatal (no session / aborted / bad
 * offset or length), -2 busy (no evictable slot right now). Busy is NOT a
 * failure: the caller must reply "busy" and let the browser retry, so the
 * socket keeps flowing and parked readers keep being served. A waited-on
 * write practically never reports busy: with any reader parked there is
 * always an unshielded slot (readers << slots), and a free slot short-
 * circuits before eviction. */
int ws_live_try_write(uint64_t offset, const void *data, size_t len) {
    if (!data && len > 0) return -1;
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active || g_lv.aborted) { pthread_mutex_unlock(&g_lv.mu); return -1; }
    if (len == 0) { pthread_mutex_unlock(&g_lv.mu); return 0; }
    if (offset + len > g_lv.total || offset % SEG != 0) {
        pthread_mutex_unlock(&g_lv.mu);
        return -1;
    }
    uint64_t first_seg = offset / SEG;
    uint64_t last_end = offset + len;
    uint64_t last_seg = (last_end - 1) / SEG;
    for (uint64_t s = first_seg; s <= last_seg; s++) {
        int r = store_seg_locked(s, data, offset, len);
        if (r == 1) continue; /* no overlap: nothing to do */
        if (r == -1) { pthread_mutex_unlock(&g_lv.mu); return -1; }
        if (r == -2) {
            /* Earlier segs of this call (if any) stay stored; the browser
             * retries the whole message, which is idempotent. */
            pthread_mutex_unlock(&g_lv.mu);
            return -2;
        }
    }
    pthread_cond_broadcast(&g_lv.changed);
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

/* Total parked-reader count across all segments (0 when no session).
 * The WS worker uses it to suppress the socket idle kill while the
 * installer is waiting on seeks. */
int ws_live_waiter_count(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    uint64_t n = 0;
    if (g_lv.active && g_lv.waiters) {
        for (uint64_t s = 0; s < g_lv.nsegs; s++)
            n += (uint64_t)g_lv.waiters[s];
    }
    pthread_mutex_unlock(&g_lv.mu);
    return (int)n;
}

int ws_live_finish(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    /* Finish = every segment stored at least once (not necessarily still
     * present: consumed segments may have been evicted for later ones). */
    uint64_t acked = 0;
    for (uint64_t s = 0; s < g_lv.nsegs; s++)
        if (g_lv.acked_ever[s]) acked++;
    if (!g_lv.active || g_lv.aborted || acked != g_lv.nsegs) {
        pthread_mutex_unlock(&g_lv.mu);
        return -1;
    }
    g_lv.upload_complete = 1;
    install_log("[WS] live upload byte-complete: '%s' (%llu bytes)",
                g_lv.filename, (unsigned long long)g_lv.total);
    pthread_cond_broadcast(&g_lv.changed);
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

void ws_live_abort(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (g_lv.active && !g_lv.aborted) {
        g_lv.aborted = 1;
        if (!g_lv.upload_complete) {
            uint64_t bytes = 0;
            for (uint64_t s = 0; s < g_lv.nsegs; s++)
                if (g_lv.present[s]) bytes += seg_len_locked(s);
            install_log("[WS] live session aborted (%llu/%llu bytes present)",
                        (unsigned long long)bytes,
                        (unsigned long long)g_lv.total);
        }
    }
    pthread_cond_broadcast(&g_lv.changed);
    pthread_mutex_unlock(&g_lv.mu);
}

void ws_live_destroy(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active) { pthread_mutex_unlock(&g_lv.mu); return; }
    g_lv.active = 0;
    g_lv.aborted = 1;
    pthread_cond_broadcast(&g_lv.changed);
    if (g_lv.refs == 0) {
        free_locked();
        install_log("[WS] live session destroyed");
    } else {
        g_lv.free_pending = 1;
    }
    g_lv.upload_complete = 0;
    g_lv.session_id[0] = '\0';
    g_lv.filename[0] = '\0';
    g_lv.total = 0;
    g_lv.nsegs = 0;
    g_lv.present_count = 0;
    g_lv.served = 0;
    pthread_mutex_unlock(&g_lv.mu);
}

int ws_live_attach(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active) { pthread_mutex_unlock(&g_lv.mu); return -1; }
    g_lv.refs++;
    pthread_mutex_unlock(&g_lv.mu);
    return 0;
}

void ws_live_detach(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (g_lv.refs > 0) g_lv.refs--;
    if (g_lv.refs == 0 && g_lv.free_pending) {
        free_locked();
        install_log("[WS] live session destroyed (deferred)");
    }
    pthread_mutex_unlock(&g_lv.mu);
}

void ws_live_reset_for_tests(void) {
    ws_live_abort();
    /* Tests join readers first; force-clear any deferred state. */
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    g_lv.refs = 0;
    g_lv.free_pending = 0;
    free_locked();
    g_lv.active = 0;
    g_lv.aborted = 0;
    g_lv.upload_complete = 0;
    g_timeout_sec = WS_LIVE_TIMEOUT_DEFAULT_SEC;
    pthread_mutex_unlock(&g_lv.mu);
}

int ws_live_wait_header(int timeout_sec) {
    live_lock_init();
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += (timeout_sec > 0) ? timeout_sec : g_timeout_sec;
    pthread_mutex_lock(&g_lv.mu);
    for (;;) {
        if (!g_lv.active || g_lv.aborted) { pthread_mutex_unlock(&g_lv.mu); return -1; }
        if (g_lv.nsegs > 0 && g_lv.present[0]) { pthread_mutex_unlock(&g_lv.mu); return 0; }
        if (tick_wait(&g_lv.changed, &g_lv.mu, &dl)) {
            pthread_mutex_unlock(&g_lv.mu);
            return -1;
        }
    }
}

size_t ws_live_get_header(uint8_t *out, size_t len) {
    if (!out || len == 0) return 0;
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    size_t got = 0;
    if (g_lv.active && g_lv.nsegs > 0 && g_lv.present[0]) {
        size_t full = (size_t)seg_len_locked(0);
        got = (len < full) ? len : full;
        memcpy(out, g_lv.slots + (size_t)g_lv.slot_of[0] * SEG, got);
    }
    pthread_mutex_unlock(&g_lv.mu);
    return got;
}

/* Emit a seek request for seg (coalesced: at most one per 2 s per seg).
 * Caller holds the lock; the sink is invoked WITHOUT the lock. */
static void emit_seek_locked(uint64_t seg, ws_live_seek_fn *out_fn, uint64_t *out_seg) {
    uint64_t now = now_ms();
    if (seg < g_lv.nsegs && now - g_lv.seek_sent[seg] > 2000) {
        g_lv.seek_sent[seg] = now;
        *out_fn = g_lv.seek_fn;
        *out_seg = seg;
    } else {
        *out_fn = NULL;
    }
}

long ws_live_read(uint64_t off, void *buf, size_t count) {
    if (!buf) return -1;
    if (count == 0) return 0;
    live_lock_init();
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += g_timeout_sec;
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active) { pthread_mutex_unlock(&g_lv.mu); return -1; }
    if (off >= g_lv.total) { pthread_mutex_unlock(&g_lv.mu); return 0; }
    if (count > g_lv.total - off) count = (size_t)(g_lv.total - off);

    uint8_t *dst = (uint8_t *)buf;
    size_t got = 0;
    /* read_start is where this read begins; served_end is where it ends.
     * run_hi only advances for contiguous forward reads (read_start within
     * one segment of the mark) so a long backward probe never over-claims the
     * reader has passed regions it still needs (see alloc_slot_locked). */
    uint64_t read_start = off;
    uint64_t served_end = off + count;
    for (;;) {
        if (g_lv.aborted) { pthread_mutex_unlock(&g_lv.mu); return -1; }
        uint64_t end = off + (count - got);
        /* Find the first missing segment in [off, end). */
        uint64_t miss = end;
        for (uint64_t o = off; o < end; ) {
            uint64_t s = o / SEG;
            if (s >= g_lv.nsegs || !g_lv.present[s]) { miss = o; break; }
            uint64_t s_end = (s + 1) * SEG;
            if (s_end > g_lv.total) s_end = g_lv.total;
            o = (s_end > o) ? s_end : o + 1;
            if (o >= end) { miss = end; break; }
        }
        if (miss >= end) {
            /* Fully present: copy segment by segment (under lock, so no
             * eviction can tear the copy) and credit served. */
            while (got < count) {
                uint64_t s = off / SEG;
                int slot = (s < g_lv.nsegs) ? g_lv.slot_of[s] : -1;
                if (slot < 0) break; /* raced teardown: rescan */
                uint64_t s_start = s * SEG;
                size_t so = (size_t)(off - s_start);
                size_t m = SEG - so;
                if (m > count - got) m = count - got;
                uint64_t seg_left = seg_len_locked(s);
                if (so + m > seg_left) m = (size_t)(seg_left - so);
                if (m == 0) break;
                memcpy(dst + got, g_lv.slots + (size_t)slot * SEG + so, m);
                g_lv.last_use[slot] = ++g_lv.tick;
                g_lv.served += m;
                off += m;
                got += m;
            }
            pthread_cond_broadcast(&g_lv.changed); /* writer space */
            if (got >= count) {
                /* Advance the eviction HWM only for a contiguous forward
                 * read (read_start within one segment of the mark): this keeps
                 * run_hi a true "reader has passed this far" boundary, so a
                 * probe of an unrelated region can never make the still-needed
                 * segments evictable. Monotonic: never retracts. */
                if (read_start <= g_lv.run_hi + SEG && served_end > g_lv.run_hi)
                    g_lv.run_hi = served_end;
                pthread_mutex_unlock(&g_lv.mu);
                return (long)got;
            }
            continue; /* raced teardown: rescan */
        }
        /* Missing data at miss. At true EOF with nothing more coming... */
        if (g_lv.upload_complete) {
            /* Complete means all present; reaching here implies eviction
             * churn — re-emit need via seek path below (browser resends). */
        }
        uint64_t need_seg = miss / SEG;
        /* Far-seek HWM: the highest segment the reader has sought. A far
         * write to a segment at/above this may reclaim an unread front slot
         * (see alloc_slot_locked); a sequential write may only reclaim
         * served-past, so it can never race ahead of the reader. */
        if (need_seg > g_lv.peak_seg && need_seg < g_lv.nsegs)
            g_lv.peak_seg = need_seg;
        ws_live_seek_fn fn = NULL;
        uint64_t fseg = 0;
        /* Protect the target from eviction while we wait on it. */
        if (need_seg < g_lv.nsegs) g_lv.waiters[need_seg]++;
        /* Seg 0 is the pinned header: always delivered first and never
         * evicted, so just wait for it (like ws_live_wait_header) rather
         * than seeking; every other segment is the browser's job to fetch. */
        if (need_seg > 0)
            emit_seek_locked(need_seg, &fn, &fseg);
        if (fn) {
            pthread_mutex_unlock(&g_lv.mu);
            fn(fseg);
            pthread_mutex_lock(&g_lv.mu);
            if (!g_lv.active || g_lv.aborted) {
                if (need_seg < g_lv.nsegs && g_lv.waiters[need_seg] > 0)
                    g_lv.waiters[need_seg]--;
                pthread_mutex_unlock(&g_lv.mu);
                return -1;
            }
            /* Rescan: the segment may have landed while emitting. */
            if (need_seg < g_lv.nsegs && g_lv.present[need_seg]) {
                if (g_lv.waiters[need_seg] > 0) g_lv.waiters[need_seg]--;
                continue;
            }
        }
        int expired = tick_wait(&g_lv.changed, &g_lv.mu, &dl);
        if (need_seg < g_lv.nsegs && g_lv.waiters[need_seg] > 0)
            g_lv.waiters[need_seg]--;
        if (expired) {
            install_log("[WS] live read timed out at off=%llu",
                        (unsigned long long)off);
            pthread_mutex_unlock(&g_lv.mu);
            return -1;
        }
    }
}

int ws_live_get_status(char *out_json, size_t max) {
    if (!out_json || max == 0) return -1;
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    if (!g_lv.active) {
        snprintf(out_json, max,
            "{\"active\":false,\"aborted\":%s,\"session_id\":\"\",\"filename\":\"\","
            "\"total\":0,\"received\":0,\"served\":0,\"complete\":false,"
            "\"header_ready\":false,\"slots_used\":0,\"slots\":0}",
            g_lv.aborted ? "true" : "false");
        pthread_mutex_unlock(&g_lv.mu);
        return 0;
    }
    uint64_t bytes = 0;
    uint64_t resident = 0;
    for (uint64_t s = 0; s < g_lv.nsegs; s++)
    {
        if (g_lv.acked_ever[s]) bytes += seg_len_locked(s);
        if (g_lv.present[s]) resident += seg_len_locked(s);
    }
    int header_ready = g_lv.active && !g_lv.aborted && g_lv.nsegs > 0 && g_lv.present[0];
    uint64_t slots_used = 0;
    char escaped_filename[512];
    json_filename(g_lv.filename, escaped_filename, sizeof(escaped_filename));
    for (int i = 0; i < g_lv.nslots; i++)
        if (g_lv.slot_seg[i] >= 0) slots_used++;
    int n = snprintf(out_json, max,
        "{\"active\":%s,\"aborted\":%s,\"session_id\":\"%s\",\"filename\":\"%s\","
        "\"total\":%llu,\"received\":%llu,\"served\":%llu,\"complete\":%s,"
        "\"header_ready\":%s,\"resident\":%llu,\"slots_used\":%llu,\"slots\":%d}",
        g_lv.active ? "true" : "false",
        g_lv.aborted ? "true" : "false",
        g_lv.session_id, escaped_filename,
        (unsigned long long)g_lv.total, (unsigned long long)bytes,
        (unsigned long long)g_lv.served,
        (g_lv.active && g_lv.upload_complete) ? "true" : "false",
        header_ready ? "true" : "false", (unsigned long long)resident,
        (unsigned long long)slots_used, g_lv.nslots);
    pthread_mutex_unlock(&g_lv.mu);
    return (n > 0 && (size_t)n < max) ? 0 : -1;
}

uint64_t ws_live_get_total(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    uint64_t t = g_lv.active ? g_lv.total : 0;
    pthread_mutex_unlock(&g_lv.mu);
    return t;
}

void ws_live_get_counters(uint64_t *out_received, uint64_t *out_total) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    uint64_t bytes = 0;
    if (g_lv.active && g_lv.present) {
        for (uint64_t s = 0; s < g_lv.nsegs; s++)
            if (g_lv.present[s]) bytes += seg_len_locked(s);
    }
    if (out_received) *out_received = bytes;
    if (out_total) *out_total = g_lv.active ? g_lv.total : 0;
    pthread_mutex_unlock(&g_lv.mu);
}

uint64_t ws_live_get_resume_offset(void) {
    live_lock_init();
    pthread_mutex_lock(&g_lv.mu);
    uint64_t s = 0;
    if (g_lv.active && g_lv.acked_ever)
        while (s < g_lv.nsegs && g_lv.acked_ever[s]) s++;
    uint64_t bytes = s * SEG;
    if (bytes > g_lv.total) bytes = g_lv.total;
    pthread_mutex_unlock(&g_lv.mu);
    return bytes;
}
