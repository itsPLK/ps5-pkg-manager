/* The embedded helper owns one AppInstUtil session and at most one install.
 * It deliberately has no dependency on the daemon's servers or global state. */
#include "install_ipc.h"
#include "install_appinst.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__Prospero__) || defined(PS5_BUILD)
#include <ps5/kernel.h>
#include <sys/syscall.h>
#endif

static void helper_phase_log(const char *fmt, ...) {
    mkdir("/data/pkgmgr", 0777);
    int fd = open("/data/pkgmgr/helper-log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        size_t size = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        if (fd >= 0) {
            write(fd, buf, size);
            fsync(fd);
            close(fd);
        }
        write(STDOUT_FILENO, buf, size);
    } else if (fd >= 0) {
        close(fd);
    }
}

typedef struct {
    int fd;
    atomic_int done;
} parent_watch_t;

/* Parent death must also retire the helper while a native call is blocked.
 * Peek only: the protocol thread remains the sole consumer of requests. */
static void *watch_parent(void *arg) {
    parent_watch_t *watch = arg;
    while (!atomic_load(&watch->done)) {
        struct pollfd pfd = {.fd = watch->fd, .events = POLLIN};
        int ret = poll(&pfd, 1, 100);
        if (ret > 0) {
            char byte;
            ssize_t n = recv(watch->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (n == 0 || (pfd.revents & (POLLHUP | POLLNVAL))) _exit(0);
            if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) _exit(0);
            usleep(10000);
        }
    }
    return NULL;
}

static int register_shortcut(const char *title_id, const char *directory) {
#if defined(__Prospero__) || defined(PS5_BUILD)
    uint32_t handle = 0;
    int (*install_title_dir)(const char *, const char *, void *) = NULL;
    if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
        install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    }
    if (install_title_dir) return install_title_dir(title_id, directory, NULL);
#else
    (void)title_id;
    (void)directory;
#endif
    return sceAppInstUtilAppInstallAll(NULL);
}

int install_helper_serve(int fd) {
    helper_phase_log("[HELPER] serve: starting fd=%d\n", fd);
    signal(SIGPIPE, SIG_IGN);
    parent_watch_t watch = {.fd = fd};
    atomic_init(&watch.done, 0);
    pthread_t watcher;
    helper_phase_log("[HELPER] serve: creating watch_parent thread...\n");
    if (pthread_create(&watcher, NULL, watch_parent, &watch) != 0) {
        helper_phase_log("[HELPER] serve: pthread_create failed errno=%d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    helper_phase_log("[HELPER] serve: watcher thread created ok\n");
    int64_t began = install_ipc_now_ms();
    errno = 0;
    helper_phase_log("[HELPER] serve: calling sceAppInstUtilInitialize()...\n");
    int initialized = sceAppInstUtilInitialize();
    int init_errno = errno;
    helper_phase_log("[HELPER] serve: sceAppInstUtilInitialize() returned 0x%08X (errno=%d)\n",
                     (unsigned)initialized, init_errno);
    int terminated = 0;
    install_ipc_response_t response = {
        .magic = INSTALL_IPC_MAGIC, .version = INSTALL_IPC_VERSION,
        .op = INSTALL_IPC_READY, .result = initialized, .pid = getpid(),
        .native_ms = (uint32_t)(install_ipc_now_ms() - began), .native_errno = init_errno,
        .build_commit = PKGMGR_BUILD_COMMIT
    };
    int used = 0;
    int installed = 0;
    pkg_info_t info = {0};
    /* Keep every native argument alive and unchanged while the system may
     * still refer to it, including after subsequent status requests. */
    install_ipc_request_t install_request;
    pkg_metadata_t metadata = {0};
    playgo_info_t playgo = {0};
    helper_phase_log("[HELPER] serve: sending INSTALL_IPC_READY (bytes=%zu)...\n", sizeof(response));
    int tr_res = install_ipc_transfer(fd, &response, sizeof(response), 1, 10000, NULL);
    helper_phase_log("[HELPER] serve: send READY result=%d\n", tr_res);
    if (tr_res != 0 || initialized != 0)
        goto done;

    helper_phase_log("[HELPER] serve: entering request loop\n");
    for (;;) {
        install_ipc_request_t request;
        helper_phase_log("[HELPER] serve: waiting for request (timeout 120s)...\n");
        if (install_ipc_transfer(fd, &request, sizeof(request), 0, 120000, NULL) != 0) {
            helper_phase_log("[HELPER] serve: request transfer error, breaking\n");
            break;
        }
        helper_phase_log("[HELPER] serve: got request op=%d magic=0x%X\n", (int)request.op, (unsigned)request.magic);
        if (request.magic != INSTALL_IPC_MAGIC || request.version != INSTALL_IPC_VERSION ||
            !memchr(request.uri, 0, sizeof(request.uri)) ||
            !memchr(request.name, 0, sizeof(request.name)) ||
            !memchr(request.title_id, 0, sizeof(request.title_id)) ||
            !memchr(request.directory, 0, sizeof(request.directory))) {
            helper_phase_log("[HELPER] serve: invalid request header, breaking\n");
            break;
        }

        memset(&response, 0, sizeof(response));
        response.magic = INSTALL_IPC_MAGIC;
        response.version = INSTALL_IPC_VERSION;
        response.op = request.op;
        response.pid = getpid();
        response.result = INSTALL_SERVICE_UNAVAILABLE;
        began = install_ipc_now_ms();
        errno = 0;
        if (request.op == INSTALL_IPC_INSTALL && !used) {
            used = 1; /* Even a failed call consumes this process. */
            install_request = request;
            metadata = (pkg_metadata_t){install_request.uri, "", "", "", install_request.name, ""};
            helper_phase_log("[HELPER] serve: calling sceAppInstUtilInstallByPackage uri=%s name=%s\n",
                             install_request.uri, install_request.name);
            response.result = sceAppInstUtilInstallByPackage(&metadata, &info, &playgo);
            info.content_id[sizeof(info.content_id) - 1] = '\0';
            response.info = info;
            installed = response.result == 0;
            helper_phase_log("[HELPER] serve: sceAppInstUtilInstallByPackage returned 0x%08X cid=%s errno=%d\n",
                             (unsigned)response.result, info.content_id, errno);
        } else if (request.op == INSTALL_IPC_STATUS && installed) {
            response.result = sceAppInstUtilGetInstallStatus(info.content_id, &response.status);
            response.status.status[sizeof(response.status.status) - 1] = '\0';
        } else if (request.op == INSTALL_IPC_SHORTCUT && !used) {
            used = 1;
            response.result = register_shortcut(request.title_id, request.directory);
        } else if (request.op == INSTALL_IPC_CLOSE) {
            helper_phase_log("[HELPER] serve: terminating sceAppInstUtil\n");
            response.result = sceAppInstUtilTerminate();
            terminated = 1;
        }
        response.native_errno = errno;
        response.native_ms = (uint32_t)(install_ipc_now_ms() - began);
        if (install_ipc_transfer(fd, &response, sizeof(response), 1, 10000, NULL) != 0) break;
        if (terminated) break;
    }
done:
    if (initialized == 0 && !terminated) {
        helper_phase_log("[HELPER] serve: cleanup terminating sceAppInstUtil\n");
        sceAppInstUtilTerminate();
    }
    atomic_store(&watch.done, 1);
    pthread_join(watcher, NULL);
    close(fd);
    helper_phase_log("[HELPER] serve: exiting initialized=%d\n", initialized);
    return initialized != 0;
}

#ifndef INSTALL_HELPER_TEST
extern int sceNetInit(void);
extern int sceNetCtlInit(void);
extern int sceUserServiceInitialize(int *priority);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data/pkgmgr", 0777);
    int fd = open("/data/pkgmgr/helper-log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);

    helper_phase_log("[HELPER] entered main() pid=%d ppid=%d uid=%d\n",
                     (int)getpid(), (int)getppid(), (int)getuid());
#if defined(__Prospero__) || defined(PS5_BUILD)
    long r = syscall(SYS_thr_set_name, -1, "pkgmgr-inst.elf");
    helper_phase_log("[HELPER] thr_set_name name=pkgmgr-inst.elf result=%ld errno=%d\n",
                     r, errno);
#endif

    int netctl_result = sceNetCtlInit();
    helper_phase_log("[HELPER] sceNetCtlInit result=0x%08X errno=%d\n",
                     (unsigned)netctl_result, errno);
    int user_prio = 256;
    int user_result = sceUserServiceInitialize(&user_prio);
    helper_phase_log("[HELPER] sceUserServiceInitialize result=0x%08X errno=%d\n",
                     (unsigned)user_result, errno);
    int net_result = sceNetInit();
    helper_phase_log("[HELPER] sceNetInit result=0x%08X errno=%d\n",
                     (unsigned)net_result, errno);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(INSTALL_IPC_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    helper_phase_log("[HELPER] connecting to IPC port %d...\n", INSTALL_IPC_PORT);
    int sock = -1;
    int connected = 0;
    int last_errno = 0;
    for (int i = 0; i < 200; i++) {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                connected = 1;
                break;
            }
            last_errno = errno;
            close(sock);
            sock = -1;
        } else {
            last_errno = errno;
        }
        usleep(100000); /* 100ms */
    }

    if (!connected) {
        helper_phase_log("[HELPER] failed to connect to IPC port %d: errno=%d (%s)\n",
                         INSTALL_IPC_PORT, last_errno, strerror(last_errno));
        _exit(2);
    }

    helper_phase_log("[HELPER] connected to IPC! Calling install_helper_serve...\n");
    int ret = install_helper_serve(sock);
    helper_phase_log("[HELPER] install_helper_serve returned %d, exiting\n", ret);
    close(sock);
    _exit(ret);
}
#endif
