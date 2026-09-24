#include "install_ipc.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

int64_t install_ipc_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int install_ipc_transfer(int fd, void *buffer, size_t size, int sending,
                         int timeout_ms, install_service_canceled_fn canceled) {
    int64_t deadline = install_ipc_now_ms() + timeout_ms;
    unsigned char *cursor = buffer;
    while (size) {
        if (canceled && canceled()) {
            errno = ECANCELED;
            return INSTALL_SERVICE_CANCELED;
        }
        int64_t remaining = deadline - install_ipc_now_ms();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return INSTALL_SERVICE_TIMEOUT;
        }
        struct pollfd pfd = { .fd = fd, .events = sending ? POLLOUT : POLLIN };
        int ready = poll(&pfd, 1, remaining > 100 ? 100 : (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return INSTALL_SERVICE_DISCONNECTED;
        if (!ready) continue;
        /* A final reply may accompany POLLHUP. Drain readable bytes first. */
        if (!(pfd.revents & pfd.events)) {
            errno = (pfd.revents & POLLNVAL) ? EBADF : ECONNRESET;
            return INSTALL_SERVICE_DISCONNECTED;
        }
        ssize_t count = sending
            ? send(fd, cursor, size, MSG_DONTWAIT | MSG_NOSIGNAL)
            : recv(fd, cursor, size, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) {
            if (!count) errno = ECONNRESET;
            return INSTALL_SERVICE_DISCONNECTED;
        }
        cursor += count;
        size -= (size_t)count;
    }
    return 0;
}
