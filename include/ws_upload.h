#ifndef WS_UPLOAD_H
#define WS_UPLOAD_H

/* PKG Manager - Direct-install WebSocket upload (NEW, ISOLATED module).
 *
 * Live RAM streaming: a browser pushes .pkg bytes and the PS5 installer
 * pulls them concurrently through the virtual stream layer. NOTHING is
 * stored on disk: bytes live in the ws_stream ring (RAM) between writer
 * and readers. See include/ws_stream.h.
 *
 * Transport + protocol live here (handshake, framing, listener on :8846,
 * REST session ops); all byte storage lives in ws_stream.c.
 *
 * Design rules:
 *  - All symbols prefixed ws_direct_*. No constructor/destructor attrs.
 *  - No threads/sockets at load or at installer_init time. The listener
 *    starts lazily via ws_direct_ensure_listener() (first REST/WS use) or
 *    explicitly in tests via ws_direct_listener_start().
 *  - Single active upload session, single browser: seeks are delivered to
 *    the authenticated uploader; a second owner or socket is rejected.
 *  - Explicit per-segment addressing: every binary message belongs to the
 *    segment named by the preceding {"op":"seg"} text frame, so the
 *    browser may send (and resend) in any order, including far seeks that
 *    jump ahead of the baseline cursor.
 *  - The conn worker never blocks on storage. Segment stores go through
 *    ws_live_try_write: 0 stored (reply "ack"), -2 ring-busy (reply
 *    "busy", browser requeues and retries), anything else (reply
 *    "error", fatal). The socket therefore keeps flowing even when the
 *    ring is momentarily full of unserved data.
 *  - No dependency on libmicrohttpd (host tests link this file without MHD).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_DIRECT_DEFAULT_PORT 8846
#define WS_DIRECT_CHUNK_DEFAULT (1024 * 1024)
#define WS_DIRECT_SESSION_ID_MAX 64
#define WS_DIRECT_FILENAME_MAX 256
#define WS_DIRECT_MAX_TOTAL (256ULL * 1024 * 1024 * 1024) /* sanity cap */
#define WS_DIRECT_ICON_MAX (10 * 1024 * 1024)

/* Create a live session. out_session_id gets a hex id.
 * Returns 0 on ok, -2 if another session is active, -1 on error.
 * Starts the listener lazily. */
int ws_direct_init_session(const char *filename, uint64_t total_size,
                           char *out_session_id, size_t sid_max);
/* Browser-owned session: owner is a random per-tab token, sid is required
 * for resume. A second tab cannot claim a session by filename and size. */
int ws_direct_init_owned(const char *filename, uint64_t total_size,
                         const char *owner, const char *resume_sid,
                         char *out_session_id, size_t sid_max);
int ws_direct_owner_matches(const char *owner, const char *sid);
int ws_direct_cancel_owned(const char *owner, const char *sid);
void ws_direct_set_metadata(const char *owner, const char *sid,
                            const char *title, const char *title_id,
                            const char *version, const char *kind);
int ws_direct_get_metadata(const char *sid, char *title, size_t title_max,
                           char *title_id, size_t id_max, char *version,
                           size_t version_max, char *kind, size_t kind_max);

/* Session icon stays in RAM and is served to all browser sessions. The
 * returned icon copy belongs to the caller; free it after use. */
int ws_direct_set_icon(const char *owner, const char *sid,
                       const uint8_t *png, size_t size);
int ws_direct_get_icon(const char *sid, uint8_t **out_png, size_t *out_size);

/* Append a chunk at the current offset. Must be in-order; may block on
 * reader backpressure. Returns 0 on ok, -3 on offset mismatch
 * (*out_expected gets server offset), -1 on abort/timeout/error. */
int ws_direct_write_chunk(uint64_t offset, const void *data, size_t len,
                          uint64_t *out_expected);

/* Mark the upload byte-complete (requires received == total). out_uri gets
 * the "live:<id>" URI to hand to POST /api/install. Session stays active
 * for readers until the install ends (abort/destroy there). */
int ws_direct_finish_session(char *out_uri, size_t uri_max);

/* Abort + free the session (idempotent; safe with readers attached —
 * the free then waits for the last detach). */
void ws_direct_cancel_session(void);

/* Reset everything (for tests). */
void ws_direct_reset_for_tests(void);

int ws_direct_session_active(void);

/* JSON status (live fields: header_ready, served, ring). 0 on ok. */
int ws_direct_get_status(char *out_json, size_t max);

/* ---- RFC6455 helpers (no sockets; pure encode/decode) ---- */

/* Compute Sec-WebSocket-Accept for a client key. Returns 0 on ok. */
int ws_direct_handshake_accept_key(const char *client_key,
                                   char *out_accept, size_t max);

/* Encode one server-to-client frame (unmasked) into out (out_max bytes).
 * Returns frame length, or -1 if out too small. */
int ws_direct_frame_encode_server(unsigned char opcode,
                                  const unsigned char *payload, size_t plen,
                                  unsigned char *out, size_t out_max);

/* Encode one client-to-browser-style frame (masked, random mask) — used by
 * the host test client. Returns frame length, or -1 if out too small. */
int ws_direct_frame_encode_client(unsigned char opcode,
                                  const unsigned char *payload, size_t plen,
                                  unsigned char *out, size_t out_max);

/* Decode one frame from buf[0..buflen). On success returns bytes consumed
 * (>0), fills *out_opcode, *out_fin, *out_pay_off, *out_pay_len. Returns 0
 * if more bytes are needed, -1 on protocol error. Unmasks in place when
 * the frame was masked. */
int ws_direct_frame_decode(unsigned char *buf, size_t buflen,
                           unsigned char *out_opcode, int *out_fin,
                           size_t *out_pay_off, size_t *out_pay_len);

/* ---- Listener (raw-socket WS server, LAN-facing) ---- */

/* Start listener on port (0.0.0.0). 0 = already running. */
int ws_direct_listener_start(int port);
/* Ensure listener is running on the default port (lazy boot for daemon). */
int ws_direct_ensure_listener(void);
void ws_direct_listener_stop(void);
int ws_direct_listener_running(void);
int ws_direct_listener_port(void);

#ifdef __cplusplus
}
#endif

#endif /* WS_UPLOAD_H */
