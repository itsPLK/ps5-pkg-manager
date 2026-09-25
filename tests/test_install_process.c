/* Exercise the real ELF upload and IPC callback against a fake elfldr. */
#include "install_process.h"
#include "install_ipc.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void install_log(const char *fmt, ...) {
    (void)fmt;
}

static int listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 1) == 0);
    return fd;
}

static void *fake_elfldr(void *arg) {
    int listen_fd = *(int *)arg;
    int upload = accept(listen_fd, NULL, NULL);
    assert(upload >= 0);
    unsigned char elf[8] = {0};
    size_t got = 0;
    for (;;) {
        ssize_t n = read(upload, elf + got, sizeof(elf) - got);
        assert(n >= 0);
        if (n == 0) break; /* The upload side must be shut down. */
        got += (size_t)n;
        assert(got < sizeof(elf));
    }
    assert(got == 4 && memcmp(elf, "\177ELF", 4) == 0);
    assert(write(upload, "helper started\n", 15) == 15);

    int callback = socket(AF_INET, SOCK_STREAM, 0);
    assert(callback >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(INSTALL_IPC_PORT)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(callback, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(callback);
    close(upload);
    return NULL;
}

int main(void) {
    int elfldr = listener(ELFLDR_PORT);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, fake_elfldr, &elfldr) == 0);
    int ipc_fd = -1;
    assert(install_process_start(&ipc_fd) == 0);
    assert(ipc_fd >= 0);
    close(ipc_fd);
    assert(pthread_join(worker, NULL) == 0);
    close(elfldr);

    ipc_fd = 123;
    assert(install_process_start(&ipc_fd) == -1);
    assert(ipc_fd == -1);
    puts("Install launcher: ELF upload, half-close, IPC callback and missing elfldr passed");
    return 0;
}
