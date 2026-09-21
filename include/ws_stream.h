#ifndef WS_STREAM_H
#define WS_STREAM_H

/* PKG Manager - live streaming session (RAM only, no disk).
 *
 * Demand-paged segment cache: the file is split into 1 MB segments; RAM
 * holds up to WS_LIVE_SLOTS of them (default 64 = 64 MB) plus pinned
 * segment 0 (header). The browser is the backing store: readers that block
 * on a missing segment emit a "seek" request (coalesced) telling the
 * browser to send that segment next. Sequential bulk never seeks; probe
 * reads (header re-reads, mid/tail table reads seen on real consoles)
 * fetch on demand. Eviction is LRU, skipping pinned/waited segments.
 *
 * Writers never drop bytes: a blocking writer (ws_live_write) waits on
 * reader backpressure, and the non-blocking variant (ws_live_try_write)
 * reports busy so the WS worker can reply "busy" and let the browser
 * retry -- either way the single conn worker never wedges. Any segment
 * may land any time; completion = every segment stored at least once
 * (acked_ever), not necessarily still resident. The browser must
 * therefore stay connected past 100% acked and keep serving resend
 * seeks until the install finalizes (single session, single browser:
 * seeks go to the latest connected uploader only).
 * Every reader wait still ends on abort/finish/timeout, so cancel and
 * session-stop always make progress.
 *
 * All symbols prefixed ws_live_*. No constructors, no threads of its own.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Segment granularity: matches browser message size, so every message
 * fills exactly one segment (except a short final segment). Segment 0 is
 * the pinned header cache (covers all observed 0-65535 re-reads) and the
 * bytes pkg_parser_parse_mem needs. */
#define WS_LIVE_SEG_SIZE (1024 * 1024)

/* Default RAM slots (env WS_LIVE_SLOTS; tests use small values to force
 * eviction/seek traffic). */
#define WS_LIVE_SLOTS_DEFAULT 64

/* Default bound for any single blocking wait (env WS_LIVE_TIMEOUT_SEC). */
#define WS_LIVE_TIMEOUT_DEFAULT_SEC 300

/* Seek request sink: the transport registers a function the session calls
 * (coalesced) when a reader needs a missing segment. NULL = no emission
 * (readers just wait; used by tests that pre-fill). */
typedef void (*ws_live_seek_fn)(uint64_t seg);
void ws_live_set_seek_fn(ws_live_seek_fn fn);

int ws_live_set_timeout_sec(int sec);
int ws_live_set_slots(int slots);

/* Create the single live session. ring_mb<=0 selects default/env.
 * out_session_id gets a hex id. 0 ok, -2 busy, -1 error. */
int ws_live_create(const char *filename, uint64_t total_size, int ring_mb,
                   char *out_session_id, size_t sid_max);

int ws_live_session_active(void);
/* 1 when id matches the current (active or completed) session. */
int ws_live_check_id(const char *session_id);

/* Store one message. Segment-aligned full messages, or the short final
 * segment (offset+len == total). Out-of-order allowed. 0 ok, -1 error/
 * aborted, -3 (kept for API compat, unused: any offset accepted). */
int ws_live_write(uint64_t offset, const void *data, size_t len,
                  uint64_t *out_expected);

/* Non-blocking store attempt for the WS conn worker: never waits on
 * readers, so one unstoreable segment cannot wedge the socket. Returns
 * 0 stored, -1 fatal (no session / aborted / bad offset or length),
 * -2 busy (no evictable slot right now -- reply "busy" and let the
 * browser retry; never treat as failure). */
int ws_live_try_write(uint64_t offset, const void *data, size_t len);

/* Total parked-reader count across all segments (0 when no session).
 * Used to suppress the socket idle kill while the installer waits. */
int ws_live_waiter_count(void);

/* Mark the upload byte-complete (requires every segment present). */
int ws_live_finish(void);
/* Unblock every waiter with failure (idempotent). */
void ws_live_abort(void);
/* Free session resources (idempotent; safe with readers attached — the
 * free then waits for the last detach). */
void ws_live_destroy(void);
void ws_live_reset_for_tests(void);

/* Block until segment 0 is present (parse-ready), abort, or timeout. */
int ws_live_wait_header(int timeout_sec);
/* Copy cached prefix bytes (only what arrived, up to len). */
size_t ws_live_get_header(uint8_t *out, size_t len);

/* Blocking range read: delivers exactly count bytes unless at true EOF
 * (returns short/0) or on abort/timeout (returns -1). Never returns
 * partial mid-file data. Missing segments trigger seek emission. */
long ws_live_read(uint64_t off, void *buf, size_t count);

/* JSON status (active, session, total, present_bytes, served, complete,
 * header_ready, slots). Always NUL-terminated. 0 on ok. */
int ws_live_get_status(char *out_json, size_t max);

/* Snapshots for progress replies. */
uint64_t ws_live_get_total(void);
void ws_live_get_counters(uint64_t *out_present, uint64_t *out_total);
/* Number of bytes in the contiguous prefix acknowledged at least once. */
uint64_t ws_live_get_resume_offset(void);

/* Reader refcount for the streaming engine (virtual_stream open/close). */
int ws_live_attach(void);
void ws_live_detach(void);

#ifdef __cplusplus
}
#endif

#endif /* WS_STREAM_H */
