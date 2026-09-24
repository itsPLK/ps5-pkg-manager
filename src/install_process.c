#include "install_process.h"
#include "install_ipc.h"
#include "installer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern const uint8_t install_helper_elf[];
extern const uint8_t install_helper_elf_end[];

int install_process_start(int *ipc_fd) {
    if (!ipc_fd) return -1;
    *ipc_fd = -1;

    size_t elf_size = (size_t)(install_helper_elf_end - install_helper_elf);
    if (elf_size == 0) {
        install_log("[HELPER] embedded helper ELF is empty");
        return -1;
    }

    /* 1. Create loopback TCP listener for the helper to connect back to */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        install_log("[HELPER] IPC listen socket failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    sin.sin_port = htons(INSTALL_IPC_PORT);

    if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        install_log("[HELPER] IPC bind to 127.0.0.1:%d failed: %s", INSTALL_IPC_PORT, strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) != 0) {
        install_log("[HELPER] IPC listen failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    /* 2. Connect to elfldr on 127.0.0.1:9021 */
    int elfldr_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (elfldr_fd < 0) {
        install_log("[HELPER] elfldr socket failed: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(ELFLDR_PORT);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");

    install_log("[HELPER] connecting to elfldr at 127.0.0.1:%d...", ELFLDR_PORT);
    if (connect(elfldr_fd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        install_log("[HELPER] connect to elfldr (127.0.0.1:%d) failed: %s (is elfldr running?)",
                    ELFLDR_PORT, strerror(errno));
        close(elfldr_fd);
        close(listen_fd);
        return -1;
    }

    /* 3. Send helper ELF to elfldr */
    install_log("[HELPER] delivering helper ELF (%zu bytes) to elfldr...", elf_size);
    size_t sent = 0;
    while (sent < elf_size) {
        ssize_t n = send(elfldr_fd, install_helper_elf + sent, elf_size - sent, 0);
        if (n <= 0) {
            install_log("[HELPER] send to elfldr failed at %zu/%zu bytes: %s",
                        sent, elf_size, strerror(errno));
            close(elfldr_fd);
            close(listen_fd);
            return -1;
        }
        sent += (size_t)n;
    }

    install_log("[HELPER] helper ELF successfully delivered to elfldr");

    /* elfldr uses this connection for the spawned process's stdio. Finish
     * the upload direction now while keeping the read side open for output. */
    if (shutdown(elfldr_fd, SHUT_WR) != 0) {
        install_log("[HELPER] shutdown upload socket failed: %s", strerror(errno));
        close(elfldr_fd);
        close(listen_fd);
        return -1;
    }

    /* Set elfldr_fd to non-blocking so reads don't stall */
    int flags = fcntl(elfldr_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(elfldr_fd, F_SETFL, flags | O_NONBLOCK);

    /* 4. Wait for the helper process to connect back to our listener while capturing output */
    install_log("[HELPER] waiting for helper connection on 127.0.0.1:%d...", INSTALL_IPC_PORT);
    int conn_fd = -1;
    int64_t wait_start = install_ipc_now_ms();
    while (install_ipc_now_ms() - wait_start < 25000) {
        struct pollfd pfds[2];
        int nfds = 1;
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        if (elfldr_fd >= 0) {
            pfds[1].fd = elfldr_fd;
            pfds[1].events = POLLIN | POLLHUP | POLLERR;
            nfds = 2;
        }

        int pr = poll(pfds, nfds, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            install_log("[HELPER] poll error: %s", strerror(errno));
            break;
        }

        if (elfldr_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char out_buf[1024];
            ssize_t nr = read(elfldr_fd, out_buf, sizeof(out_buf) - 1);
            if (nr > 0) {
                out_buf[nr] = '\0';
                char *saveptr = NULL;
                char *line = strtok_r(out_buf, "\r\n", &saveptr);
                while (line) {
                    install_log("[HELPER_ELFLDR_OUT] %s", line);
                    line = strtok_r(NULL, "\r\n", &saveptr);
                }
            } else if (nr == 0 || (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                close(elfldr_fd);
                elfldr_fd = -1;
            }
        }

        if (pfds[0].revents & POLLIN) {
            conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd >= 0) {
                fcntl(conn_fd, F_SETFD, FD_CLOEXEC);
                install_log("[HELPER] helper connected on IPC socket fd=%d", conn_fd);
                break;
            }
        }
    }

    if (elfldr_fd >= 0) {
        close(elfldr_fd);
        elfldr_fd = -1;
    }
    close(listen_fd);

    if (conn_fd < 0) {
        install_log("[HELPER] wait for helper connection timed out");
        return -1;
    }

    *ipc_fd = conn_fd;
    return 0;
}
