/* Runs the real helper protocol in exec'd host processes. Only console API
 * calls and the platform launcher are replaced; daemon IPC is unchanged. */
#include "install_service.h"
#include "install_ipc.h"
#include "install_appinst.h"
#include "install_process.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static char *executable;
static pid_t last_spawn;
static int submissions;
static int initialized;
static const pkg_metadata_t *submitted_metadata;
static int64_t cancel_at;
static char diagnostics[65536];
static size_t diagnostics_size;

void install_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int count = vsnprintf(diagnostics + diagnostics_size, sizeof(diagnostics) - diagnostics_size, fmt, args);
    va_end(args);
    assert(count > 0 && (size_t)count < sizeof(diagnostics) - diagnostics_size);
    diagnostics_size += (size_t)count;
}

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int mode(const char *value) {
    const char *env = getenv("PKGMGR_TEST_HELPER_MODE");
    return env && strcmp(env, value) == 0;
}

int sceAppInstUtilInitialize(void) {
    assert(!initialized);
    initialized = 1;
    return mode("init_fail") ? (int)0x80A30001u : 0;
}

int sceAppInstUtilTerminate(void) {
    assert(initialized);
    initialized = 0;
    if (mode("hang_terminate")) raise(SIGSTOP);
    return 0;
}

int sceAppInstUtilInstallByPackage(const pkg_metadata_t *meta, pkg_info_t *info,
                                  playgo_info_t *playgo) {
    assert(initialized && playgo);
    assert(strcmp(meta->uri, "http://127.0.0.1/package.pkg") == 0);
    assert(strcmp(meta->content_name, "Fixture") == 0);
    assert(meta->ex_uri[0] == 0 && meta->content_id[0] == 0);
    submitted_metadata = meta;
    if (++submissions > 1 || mode("slot_error")) return (int)0x80B2116Fu;
    if (mode("hang_install")) sleep(30);
    snprintf(info->content_id, sizeof(info->content_id), "TEST-%d", getpid());
    info->type = 7;
    info->platform = 5;
    return 0;
}

int sceAppInstUtilGetInstallStatus(const char *content_id, SceAppInstallStatusInstalled *status) {
    char expected[48];
    snprintf(expected, sizeof(expected), "TEST-%d", getpid());
    assert(strcmp(content_id, expected) == 0);
    assert(initialized && submissions == 1);
    assert(strcmp(submitted_metadata->uri, "http://127.0.0.1/package.pkg") == 0);
    assert(strcmp(submitted_metadata->content_name, "Fixture") == 0);
    if (mode("status_error")) return (int)0x80A30003u;
    strcpy(status->status, "completed");
    status->downloaded_size = 123456789;
    if (mode("native_status_error")) {
        strcpy(status->status, "error");
        status->error_info.error_code = (int)0x80B2116Fu;
        status->error_info.version = 1;
        strcpy(status->error_info.type, "PlayGo");
        strcpy(status->error_info.description, "Native diagnostic detail");
    }
    return 0;
}

int sceAppInstUtilAppInstallAll(void *reserved) {
    assert(initialized && !submissions && !reserved);
    return 0;
}

int install_process_start(int *ipc_fd) {
    if (mode("spawn_fail")) return -1;
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(fcntl(pair[0], F_SETFD, FD_CLOEXEC) == 0);
    assert(fcntl(pair[1], F_SETFD, FD_CLOEXEC) == 0);
    posix_spawn_file_actions_t actions;
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_adddup2(&actions, pair[1], INSTALL_IPC_FD) == 0);
    if (pair[1] != INSTALL_IPC_FD)
        assert(posix_spawn_file_actions_addclose(&actions, pair[1]) == 0);
    char *argv[] = {executable, "--helper", NULL};
    pid_t pid;
    int ret = posix_spawn(&pid, executable, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pair[1]);
    if (ret != 0) {
        close(pair[0]);
        return -1;
    }
    *ipc_fd = pair[0];
    last_spawn = pid;
    return 0;
}

static int canceled(void) { return now_ms() >= cancel_at; }

static int start(install_service_t *service, pkg_info_t *info,
                  install_service_canceled_fn cancel_fn) {
    return install_service_start(service, "http://127.0.0.1/package.pkg", "Fixture", info, cancel_fn);
}

static void close_reaped(install_service_t *service) {
    pid_t pid = service->pid;
    install_service_close(service);
    assert(service->pid == -1 && service->fd == -1);
    if (pid > 0) {
        errno = 0;
        assert(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
    }
    install_service_close(service); /* idempotent */
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--helper") == 0) {
        if (mode("bad_ready")) {
            install_ipc_response_t bad = {0};
            install_ipc_transfer(INSTALL_IPC_FD, &bad, sizeof(bad), 1, 1000, NULL);
            _exit(0);
        }
        _exit(install_helper_serve(INSTALL_IPC_FD));
    }
    executable = argv[0];
    pid_t parent = getpid(), previous = -1;
    pkg_info_t info;
    SceAppInstallStatusInstalled status;

    /* Multiple installs, with the same parent and new process state each time. */
    for (int i = 0; i < 3; ++i) {
        install_service_t service = INSTALL_SERVICE_INIT;
        assert(start(&service, &info, NULL) == 0);
        assert(service.pid > 0 && service.pid != parent && service.pid != previous);
        previous = service.pid;
        assert(info.type == 7 && info.platform == 5);
        assert(install_service_status(&service, &status) == 0);
        assert(strcmp(status.status, "completed") == 0 && status.downloaded_size == 123456789);
        /* A second submission is rejected even if a caller bypasses the client. */
        install_ipc_request_t request = {
            .magic = INSTALL_IPC_MAGIC, .version = INSTALL_IPC_VERSION, .op = INSTALL_IPC_INSTALL
        };
        install_ipc_response_t response;
        assert(install_ipc_transfer(service.fd, &request, sizeof(request), 1, 1000, NULL) == 0);
        assert(install_ipc_transfer(service.fd, &response, sizeof(response), 0, 1000, NULL) == 0);
        assert(response.result == INSTALL_SERVICE_UNAVAILABLE);
        close_reaped(&service);
    }
    assert(getpid() == parent && !initialized && !submissions);
    assert(install_service_shortcut("PKGM00001", "/user/app/") == 0);
    assert(waitpid(last_spawn, NULL, WNOHANG) == -1 && errno == ECHILD);

    const char *failures[] = {"slot_error", "init_fail", "spawn_fail", "bad_ready"};
    const int errors[] = {(int)0x80B2116Fu, (int)0x80A30001u,
                          INSTALL_SERVICE_UNAVAILABLE, INSTALL_SERVICE_DISCONNECTED};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        install_service_t service = INSTALL_SERVICE_INIT;
        setenv("PKGMGR_TEST_HELPER_MODE", failures[i], 1);
        assert(start(&service, &info, NULL) == errors[i]);
        close_reaped(&service);
        unsetenv("PKGMGR_TEST_HELPER_MODE");
        assert(start(&service, &info, NULL) == 0);
        close_reaped(&service);
    }

    install_service_t service = INSTALL_SERVICE_INIT;
    assert(start(&service, &info, NULL) == 0);
    assert(kill(service.pid, SIGKILL) == 0);
    assert(install_service_status(&service, &status) == INSTALL_SERVICE_DISCONNECTED);
    close_reaped(&service);

    setenv("PKGMGR_TEST_HELPER_MODE", "status_error", 1);
    assert(start(&service, &info, NULL) == 0);
    assert(install_service_status(&service, &status) == (int)0x80A30003u);
    close_reaped(&service);

    setenv("PKGMGR_TEST_HELPER_MODE", "native_status_error", 1);
    assert(start(&service, &info, NULL) == 0);
    assert(install_service_status(&service, &status) == 0);
    assert(status.error_info.error_code == (int)0x80B2116Fu);
    assert(strstr(diagnostics, "native_error=0x80B2116F"));
    assert(strstr(diagnostics, "Native diagnostic detail"));
    close_reaped(&service);

    setenv("PKGMGR_TEST_HELPER_MODE", "hang_install", 1);
    int64_t began = now_ms();
    cancel_at = began + 300;
    assert(start(&service, &info, canceled) == INSTALL_SERVICE_CANCELED);
    close_reaped(&service);
    assert(now_ms() - began < 3000);

    /* Losing the parent socket also exits a helper without a CLOSE command. */
    unsetenv("PKGMGR_TEST_HELPER_MODE");
    assert(start(&service, &info, NULL) == 0);
    close(service.fd);
    service.fd = -1;
    int wait_status = 0;
    pid_t exited = 0;
    for (int i = 0; i < 100 && exited == 0; ++i) {
        exited = waitpid(service.pid, &wait_status, WNOHANG);
        usleep(10000);
    }
    assert(exited == service.pid && WIFEXITED(wait_status));
    close_reaped(&service);

    setenv("PKGMGR_TEST_HELPER_MODE", "hang_terminate", 1);
    assert(start(&service, &info, NULL) == 0);
    began = now_ms();
    close_reaped(&service);
    assert(now_ms() - began < 3000);
    unsetenv("PKGMGR_TEST_HELPER_MODE");

    /* A quiet peer cannot stall IPC forever; a partial response followed by
     * EOF must be rejected, rather than read as a successful native result. */
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    char bytes[16];
    assert(install_ipc_transfer(pair[0], bytes, sizeof(bytes), 0, 30, NULL) == INSTALL_SERVICE_TIMEOUT);
    assert(write(pair[1], "half", 4) == 4);
    close(pair[1]);
    assert(install_ipc_transfer(pair[0], bytes, sizeof(bytes), 0, 1000, NULL) == INSTALL_SERVICE_DISCONNECTED);
    close(pair[0]);

    assert(strstr(diagnostics, "[HELPER] spawning parent_pid="));
    assert(strstr(diagnostics, "helper_build="));
    assert(strstr(diagnostics, "[HELPER_STATUS]"));
    assert(strstr(diagnostics, "native_ms="));
    assert(strstr(diagnostics, "receive failed"));
    assert(strstr(diagnostics, "invalid response"));
    assert(strstr(diagnostics, "reaped pid="));
    assert(strstr(diagnostics, "forced cleanup"));

    puts("Install helper: fresh processes, IPC, native errors, cancellation, death and reaping passed");
    return 0;
}
