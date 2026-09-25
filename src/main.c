/*
 * PKG Manager - Main Entry Point
 *
 * Native PS5 ELF daemon for scanning, inspecting, and installing
 * PS4 and PS5 packages with a web-based user interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "version.h"
#include "pkg_scanner.h"
#include "installer.h"
#include "http_server.h"
#include "ws_upload.h"
#include "notification.h"
#include "app_installer.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#if defined(__Prospero__) || defined(PS5_BUILD)
#include <sys/sysctl.h>
#include <sys/syscall.h>

extern int sceNetCtlInit(void);
extern int sceUserServiceInitialize(int *priority);

static pid_t find_pid(const char *name) {
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    pid_t pid = -1;
    size_t buf_size;
    uint8_t *buf;

    if (sysctl(mib, 4, 0, &buf_size, 0, 0)) {
        printf("[PKG Manager] sysctl failed\n");
        return -1;
    }

    if (!(buf = malloc(buf_size))) {
        printf("[PKG Manager] malloc failed\n");
        return -1;
    }

    if (sysctl(mib, 4, buf, &buf_size, 0, 0)) {
        printf("[PKG Manager] sysctl failed\n");
        free(buf);
        return -1;
    }

    for (uint8_t *ptr = buf; ptr < buf + buf_size;) {
        size_t remaining = (size_t)(buf + buf_size - ptr);
        int ki_structsize;
        if (remaining < sizeof(ki_structsize)) break;
        memcpy(&ki_structsize, ptr, sizeof(ki_structsize));
        /* The kinfo_proc layout may differ across firmware versions. Never
         * loop forever or read beyond a record if sysctl returns bad data. */
        if (ki_structsize < 448 || (size_t)ki_structsize > remaining) {
            printf("[PKG Manager] Invalid process record size %d\n", ki_structsize);
            break;
        }
        pid_t ki_pid;
        memcpy(&ki_pid, ptr + 72, sizeof(ki_pid));
        const char *ki_tdname = (const char *)ptr + 447;

        ptr += ki_structsize;
        if (memchr(ki_tdname, '\0', (size_t)ki_structsize - 447) &&
            !strcmp(name, ki_tdname) && ki_pid != mypid) {
            pid = ki_pid;
        }
    }

    free(buf);
    return pid;
}
#endif

static int get_local_ip(char *ip_buf, size_t buf_size) {
    struct ifaddrs *ifaddr, *ifa;
    int family, s;

    if (getifaddrs(&ifaddr) == -1) {
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET) {
            if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;

            s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                           ip_buf, buf_size, NULL, 0, NI_NUMERICHOST);
            if (s == 0) {
                if (strcmp(ip_buf, "127.0.0.1") != 0 && strcmp(ip_buf, "0.0.0.0") != 0) {
                    freeifaddrs(ifaddr);
                    return 0;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    return -1;
}

#define DEFAULT_HTTP_PORT 8844

static volatile int g_running = 1;
static volatile sig_atomic_t g_resumed = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void handle_sigcont(int sig) {
    (void)sig;
    g_resumed = 1;
}

__attribute__((used)) volatile const char pkgmgr_version_sig[] = "PKGMGR_VER:" PKGMGR_VERSION;

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* Write directly to stdout before any service call. Payload loaders may
     * capture stdout even when the process fails before printf can flush. */
    static const char entered_main[] = "[PKG Manager] entered main\n";
    (void)write(STDOUT_FILENO, entered_main, sizeof(entered_main) - 1);

#if defined(__Prospero__) || defined(PS5_BUILD)
    syscall(SYS_thr_set_name, -1, "pkgmgr.elf");

    pid_t old_pid;
    while ((old_pid = find_pid("pkgmgr.elf")) > 0) {
        if (kill(old_pid, SIGKILL)) {
            printf("[PKG Manager] kill failed\n");
            ps5_notify("PKG Manager: could not stop previous process (%d)", (int)old_pid);
            return EXIT_FAILURE;
        }
        sleep(1);
    }
    static const char process_check_done[] = "[PKG Manager] process check complete\n";
    (void)write(STDOUT_FILENO, process_check_done, sizeof(process_check_done) - 1);
#endif

    printf("[PKG Manager] Starting PKG Manager v%s (%s, %s)...\n",
           PKGMGR_VERSION, PKGMGR_BUILD_COMMIT, PKGMGR_BUILD_DATE);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCONT, handle_sigcont);

#if defined(__Prospero__) || defined(PS5_BUILD)
    printf("[PKG Manager] Initializing PS5 system services...\n");
    int net_result = sceNetCtlInit();
    if (net_result == 0) {
        printf("[PKG Manager] Network controller initialized.\n");
    } else {
        printf("[PKG Manager] sceNetCtlInit returned 0x%08X\n", net_result);
        ps5_notify("PKG Manager: network init returned 0x%08X", net_result);
    }
    int user_prio = 256;
    int user_result = sceUserServiceInitialize(&user_prio);
    if (user_result == 0) {
        printf("[PKG Manager] User service initialized.\n");
    } else {
        printf("[PKG Manager] sceUserServiceInitialize returned 0x%08X\n", user_result);
        ps5_notify("PKG Manager: user service init returned 0x%08X", user_result);
    }
#endif
    static const char services_done[] = "[PKG Manager] service initialization complete\n";
    (void)write(STDOUT_FILENO, services_done, sizeof(services_done) - 1);
    ps5_notify("PKG Manager v%s starting...", PKGMGR_VERSION);

    int port = DEFAULT_HTTP_PORT;
    char server_url[128];
    snprintf(server_url, sizeof(server_url), "http://127.0.0.1:%d/", port);

    printf("[PKG Manager] Initializing installer subsystem...\n");
    if (installer_init(server_url) != 0) {
        fprintf(stderr, "[PKG Manager] Failed to initialize installer subsystem!\n");
        ps5_notify("PKG Manager: installer initialization failed");
        return 1;
    }
    install_log("[PKG Manager] Starting PKG Manager v%s (%s, %s)...",
                PKGMGR_VERSION, PKGMGR_BUILD_COMMIT, PKGMGR_BUILD_DATE);

    printf("[PKG Manager] Initializing package scanner (%s & %s)...\n", PKG_DEFAULT_DIR, PKG_DISC_DIR);
    pkg_scanner_init();

    printf("[PKG Manager] Starting HTTP server on port %d...\n", port);
    if (http_server_start(port) != 0) {
        fprintf(stderr, "[PKG Manager] Failed to start HTTP server on port %d!\n", port);
        ps5_notify("PKG Manager: HTTP server failed to start on port %d", port);
        installer_shutdown();
        return 1;
    }

    printf("[PKG Manager] Verifying PS5 home screen shortcut...\n");
    if (app_installer_install_if_needed() != 0) {
        fprintf(stderr, "[PKG Manager] Failed to install home screen shortcut!\n");
    }

    int found_count = 0;
    if (pkg_scanner_has_manifest()) {
        found_count = (int)pkg_scanner_get_count();
        printf("[PKG Manager] Loaded %d package(s) on startup from cache manifest.\n", found_count);
    } else {
        ps5_notify("PKG Manager: server ready on port %d; scanning packages...", port);
        found_count = pkg_scanner_scan();
        printf("[PKG Manager] Initial scan found %d package(s) on startup.\n", found_count);
    }

    char current_ip[64] = "unknown";
    if (get_local_ip(current_ip, sizeof(current_ip)) != 0) {
        strcpy(current_ip, "unknown");
    }

    if (strcmp(current_ip, "unknown") != 0) {
        ps5_notify("PKG Manager v%s\nFound %d package(s)\nhttp://%s:%d",
                   PKGMGR_VERSION, found_count, current_ip, port);
    } else {
        ps5_notify("PKG Manager v%s\nFound %d package(s)\nPort: %d",
                   PKGMGR_VERSION, found_count, port);
    }

    printf("[PKG Manager] Running. Press Ctrl+C or kill process to terminate.\n");

    /* Watchdog and main loop */
    int network_check_timer = 0;
    while (g_running) {
        usleep(100000); /* 100ms sleep */

        if (http_server_exit_requested()) {
            printf("[PKG Manager] Shutdown requested from Settings.\n");
            g_running = 0;
            break;
        }

        /* Immediate Wake-up Recovery */
        if (g_resumed) {
            g_resumed = 0;
            printf("[PKG Manager] Console resumed from standby. Restarting server...\n");
            install_log("[PKG Manager] Console resumed from standby. Restarting server...");

            /* Force full server restart — close the dead socket immediately */
            http_server_stop();
            if (ws_direct_listener_running()) ws_direct_listener_stop();

            int changed = 0;
            pkg_scanner_scan_quick(NULL, &changed);

            usleep(1000000); /* 1s for network stack to stabilize */

            if (http_server_start(port) == 0) {
                /* Re-read current IP */
                if (get_local_ip(current_ip, sizeof(current_ip)) != 0) {
                    strcpy(current_ip, "unknown");
                }
                printf("[PKG Manager] Server restarted after standby. IP: %s\n", current_ip);
                install_log("[PKG Manager] Server restarted after standby. IP: %s", current_ip);
            } else {
                printf("[PKG Manager] !!! Failed to restart server after standby!\n");
                install_log("[PKG Manager] !!! Failed to restart server after standby!");
                ps5_notify("PKG Manager: Server restart failed after standby");
                strcpy(current_ip, "unknown");
            }
            /* Reset timer so we don't immediately re-check */
            network_check_timer = 0;
        }

        /* Network Watchdog (every 5 seconds) */
        if (++network_check_timer >= 50) {
            network_check_timer = 0;
            char new_ip[64] = "unknown";
            int has_ip = (get_local_ip(new_ip, sizeof(new_ip)) == 0);
            int server_up = http_server_is_running();

            watchdog_action_t action = http_server_watchdog_evaluate(server_up, has_ip, current_ip, new_ip);

            /* Discard the old upload socket after a network change. The next
             * upload request starts a fresh listener if one is needed. */
            if (action != WATCHDOG_ACTION_NONE && ws_direct_listener_running())
                ws_direct_listener_stop();

            if (action == WATCHDOG_ACTION_RESTORE_NETWORK) {
                printf("[PKG Manager] Network state refresh: %s -> %s. Restarting server...\n",
                       current_ip, new_ip);
                install_log("[PKG Manager] Network state refresh: %s -> %s. Restarting server...",
                            current_ip, new_ip);

                if (http_server_restart_with_delay(port, 800000) == 0) {
                    strcpy(current_ip, new_ip);
                    printf("[PKG Manager] Server restored on %s:%d\n", current_ip, port);
                    install_log("[PKG Manager] Server restored on %s:%d", current_ip, port);
                } else {
                    printf("[PKG Manager] !!! Failed to restore server!\n");
                    install_log("[PKG Manager] !!! Failed to restore server!");
                }
            } else if (action == WATCHDOG_ACTION_RESTORE_LOOPBACK) {
                printf("[PKG Manager] Network lost (was %s). Restarting server for loopback...\n", current_ip);
                install_log("[PKG Manager] Network lost (was %s). Restarting server for loopback...", current_ip);
                strcpy(current_ip, "unknown");

                /* Restart daemon to ensure clean socket for loopback */
                if (http_server_restart_with_delay(port, 300000) == 0) {
                    printf("[PKG Manager] Server restarted after network loss (loopback only)\n");
                    install_log("[PKG Manager] Server restarted after network loss (loopback only)");
                } else {
                    printf("[PKG Manager] !!! Failed to restart server after network loss!\n");
                    install_log("[PKG Manager] !!! Failed to restart server after network loss!");
                    ps5_notify("PKG Manager: Server restart failed");
                }
            }
        }
        ws_direct_listener_stop_if_idle();
    }

    printf("[PKG Manager] Shutting down...\n");
    http_server_stop();
    ws_direct_listener_stop();
    installer_shutdown();
    sleep(1); /* Allow sockets and OS kernel handles to close cleanly */
    printf("[PKG Manager] Exited cleanly.\n");

    return 0;
}
