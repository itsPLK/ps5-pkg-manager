#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_HTTP_PORT 8844

/**
 * Starts the HTTP server on the specified port.
 * Returns 0 on success, negative on error.
 */
int http_server_start(int port);

/**
 * Stops the HTTP server.
 */
void http_server_stop(void);

/**
 * Returns 1 if the HTTP server is currently running, 0 otherwise.
 */
int http_server_is_running(void);

/** Returns 1 after a client requested a graceful process shutdown. */
int http_server_exit_requested(void);

/**
 * Restarts the HTTP server on the specified port with a delay (microseconds)
 * between stopping and starting to allow socket cleanup and network stabilization.
 * Returns 0 on success, negative on error.
 */
int http_server_restart_with_delay(int port, unsigned int delay_us);

/**
 * Restarts the HTTP server on the specified port using a default 500ms delay.
 * Stops the server, waits briefly for socket cleanup, and restarts it.
 * Returns 0 on success, negative on error.
 */
int http_server_restart(int port);

/**
 * Action determined by the network watchdog.
 */
typedef enum {
    WATCHDOG_ACTION_NONE = 0,
    WATCHDOG_ACTION_RESTORE_NETWORK,
    WATCHDOG_ACTION_RESTORE_LOOPBACK
} watchdog_action_t;

/**
 * Evaluates whether the network watchdog needs to restore or refresh the HTTP service.
 * Handles IP changes, recovery from disconnected state, and self-healing if the server
 * died or failed to start on previous attempts.
 */
static inline watchdog_action_t http_server_watchdog_evaluate(int server_is_running,
                                                              int has_ip,
                                                              const char *current_ip,
                                                              const char *new_ip) {
    if (!current_ip) current_ip = "unknown";
    if (!new_ip) new_ip = "unknown";

    if (has_ip) {
        if (!server_is_running ||
            strcmp(new_ip, current_ip) != 0 ||
            strcmp(current_ip, "unknown") == 0) {
            return WATCHDOG_ACTION_RESTORE_NETWORK;
        }
    } else {
        if (!server_is_running ||
            strcmp(current_ip, "unknown") != 0) {
            return WATCHDOG_ACTION_RESTORE_LOOPBACK;
        }
    }

    return WATCHDOG_ACTION_NONE;
}

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_H */
