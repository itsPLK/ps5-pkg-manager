#ifndef STREAM_SERVER_H
#define STREAM_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Dedicated raw-socket HTTP server for virtual package streaming.
 * Serves byte ranges of single/multi-part packages with hand-built
 * responses (no libmicrohttpd in this path).
 */
#define STREAM_SERVER_PORT 18841

/* Opens the virtual stream for pkg_path and starts listening.
 * Returns 0 on success, negative on failure. */
int stream_server_session_start(const char *pkg_path);

/* Same, but pins the exact session filename clients must request
 * (basename of the stream URI, e.g. "package-123-1.pkg"). Requests for
 * any other name under /stream/ get 404. Pass NULL for legacy behavior
 * (any *.pkg name is served). */
int stream_server_session_start_ex(const char *pkg_path, const char *session_name);

/* Updates the pinned session filename of a running session. */
void stream_server_set_session_name(const char *session_name);

/* Stops the listener and closes the virtual stream. Safe to call idle. */
void stream_server_session_stop(void);

/* Installer retries/cancellation keep the report open until helper cleanup. */
void stream_server_session_stop_keep_log(void);

/* Returns non-zero while a session is active. */
int stream_server_is_running(void);

/* Enable or disable verbose connection logging. Disabled by default unless
 * PKG_DEBUG_STREAM environment variable is set. */
void stream_server_set_debug(int enable);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_SERVER_H */
