#ifndef STREAM_DEBUG_LOG_H
#define STREAM_DEBUG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* File-based debug logger that records EVERY connection and range request
 * from the PS5 system client to the stream server.  Each install session
 * produces one timestamped .txt file in /data/pkgmgr/ (or $PKG_DEBUG_DIR)
 * containing a reproducible record of the download pattern:
 *
 *   /data/pkgmgr/stream_debug_<title_id>_<pkg_kind>_<timestamp>_<pid>_<seq>.txt
 *
 * Intended for offline replay / mock construction of the PS5 download
 * behaviour so that remote (WebSocket) streaming can reproduce the exact
 * same byte-range access pattern.
 *
 * Thread-safe: uses its own mutex.  Calling any function when the logger
 * is not open is a safe no-op.
 */

/* Begin a new debug session.  Opens/creates the log file.
 * title_id   - e.g. "PPSA01650"  (may be "" / NULL)
 * content_id - e.g. "EP0001-PPSA01650_00-GAME000000000000" (may be "" / NULL)
 * pkg_kind   - "base", "update", "dlc", "unknown"
 * pkg_path   - full path to the source package file
 * total_size - total virtual stream size in bytes
 *
 * Returns 0 on success, -1 on failure (log will simply not be written). */
int stream_debug_log_open(const char *title_id, const char *content_id,
                          const char *pkg_kind, const char *pkg_path,
                          uint64_t total_size);

/* Record a new TCP connection accepted by the stream server. */
void stream_debug_log_conn_open(int conn_id, const char *peer);

/* Record a fully parsed HTTP request on an existing connection.
 * method      - "GET" or "HEAD"
 * path        - the raw request path (e.g. "/stream/install/package-1726765163-1.pkg")
 * range_start - first byte of range (0 for full request / no Range header)
 * range_end   - last byte of range  (total-1 for full / no Range header)
 * has_range   - 1 if a Range header was present, 0 otherwise
 * http_status - response status code we sent (200, 206, 404, 405, 416)
 * content_len - Content-Length of the response body */
void stream_debug_log_request(int conn_id, int req_no, const char *peer,
                              const char *method, const char *path,
                              int has_range, uint64_t range_start, uint64_t range_end,
                              int http_status, uint64_t content_len,
                              uint64_t total_size);

/* Record that a response body has been fully (or partially) delivered. */
void stream_debug_log_response_done(int conn_id, int req_no, const char *peer,
                                    uint64_t bytes_sent, uint64_t content_len,
                                    const char *end_reason);

/* Record binary message receive timing and successful RAM admission. */
void stream_debug_log_ws_receive(uint64_t segment, uint64_t bytes,
                                 uint64_t receive_us);
void stream_debug_log_ws_accept(uint64_t segment, uint64_t bytes);
/* Record one segment write attempt rejected because the RAM ring is busy. */
void stream_debug_log_ws_busy(uint64_t segment, uint64_t bytes);

/* Cumulative cache diagnostics: resident duplicate uploads, reuploads after
 * eviction, and evictions with no bytes read during that residency. */
void stream_debug_log_ws_cache(uint64_t duplicate_bytes, uint64_t reload_bytes,
                               uint64_t unread_evicted_bytes);

/* Browser cumulative timings. ACK latency includes transmission and overlapping
 * in-flight work; it must not be subtracted from elapsed time. */
void stream_debug_log_ws_sender(uint64_t read_us, uint64_t ack_us,
                                uint64_t acks, uint64_t sent, uint64_t window);

/* Record a connection close. */
void stream_debug_log_conn_close(int conn_id, const char *peer, int reqs_served);

/* Close the current debug session (flushes and closes the file). Safe to
 * call when no session is open. */
void stream_debug_log_close(void);

/* Returns non-zero when a debug log session is currently active. */
int stream_debug_log_is_active(void);

/* Persist an installer/helper diagnostic in the same timeline as HTTP/WS. */
void stream_debug_log_event(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_DEBUG_LOG_H */
