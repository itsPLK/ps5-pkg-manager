#include "install_service.h"
#include "install_ipc.h"
#include "install_process.h"
#include "installer.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int receive_response(install_service_t *service, uint32_t op,
                            install_ipc_response_t *response, int timeout_ms) {
    int64_t began = install_ipc_now_ms();
    int ret = install_ipc_transfer(service->fd, response, sizeof(*response), 0,
                                   timeout_ms, service->canceled);
    if (ret != 0) {
        int error = errno;
        service->usable = 0;
        install_log("[HELPER_IPC] receive failed pid=%d op=%u rc=%d errno=%d (%s) elapsed_ms=%lld timeout_ms=%d expected_bytes=%zu",
                    (int)service->pid, op, ret, error, strerror(error),
                    (long long)(install_ipc_now_ms() - began), timeout_ms, sizeof(*response));
        return ret;
    }
    if (service->pid <= 0 && op == INSTALL_IPC_READY) {
        service->pid = response->pid;
    }
    if (response->magic != INSTALL_IPC_MAGIC || response->version != INSTALL_IPC_VERSION ||
        response->op != op || response->pid != service->pid) {
        service->usable = 0;
        install_log("[HELPER_IPC] invalid response magic=0x%08X version=%u/%u op=%u/%u pid=%d/%d",
                    response->magic, response->version, INSTALL_IPC_VERSION,
                    response->op, op, response->pid, (int)service->pid);
        return INSTALL_SERVICE_DISCONNECTED;
    }
    if (op != INSTALL_IPC_STATUS) {
        install_log("[HELPER_IPC] reply pid=%d op=%u rc=0x%08X native_ms=%u native_errno=%d roundtrip_ms=%lld",
                    (int)service->pid, op, (unsigned)response->result, response->native_ms,
                    response->native_errno, (long long)(install_ipc_now_ms() - began));
    }
    return response->result;
}

static int exchange(install_service_t *service, install_ipc_request_t *request,
                     install_ipc_response_t *response, int timeout_ms) {
    request->magic = INSTALL_IPC_MAGIC;
    request->version = INSTALL_IPC_VERSION;
    int ret = install_ipc_transfer(service->fd, request, sizeof(*request), 1,
                                   10000, service->canceled);
    if (ret != 0) {
        int error = errno;
        service->usable = 0;
        install_log("[HELPER_IPC] send failed pid=%d op=%u rc=%d errno=%d (%s) bytes=%zu",
                    (int)service->pid, request->op, ret, error, strerror(error), sizeof(*request));
        return ret;
    }
    return receive_response(service, request->op, response, timeout_ms);
}

static void dump_helper_log(void) {
    int fd = open("/data/pkgmgr/helper-log.txt", O_RDONLY);
    if (fd < 0) {
        install_log("[HELPER_LOG] /data/pkgmgr/helper-log.txt not found");
        return;
    }
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        install_log("[HELPER_LOG] /data/pkgmgr/helper-log.txt is empty");
        return;
    }
    buf[n] = '\0';
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);
    while (line) {
        install_log("[HELPER_LOG] %s", line);
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
}

static int open_service(install_service_t *service, install_service_canceled_fn canceled) {
    service->canceled = canceled;
    service->usable = 0;
    service->have_status = 0;
    service->pid = -1;
    service->fd = -1;
    if (canceled && canceled()) return INSTALL_SERVICE_CANCELED;
    unlink("/data/pkgmgr/helper-log.txt");

    int64_t began = install_ipc_now_ms();
    install_log("[HELPER] spawning parent_pid=%d protocol=%u request_bytes=%zu response_bytes=%zu",
                (int)getpid(), INSTALL_IPC_VERSION, sizeof(install_ipc_request_t), sizeof(install_ipc_response_t));

    int ipc_fd = -1;
    if (install_process_start(&ipc_fd) != 0) {
        install_log("[HELPER] spawn failed errno=%d (%s) elapsed_ms=%lld", errno,
                    strerror(errno), (long long)(install_ipc_now_ms() - began));
        dump_helper_log();
        return INSTALL_SERVICE_UNAVAILABLE;
    }
    service->fd = ipc_fd;

    install_log("[HELPER] spawned; waiting for AppInstUtil initialization");
    install_ipc_response_t response = {0};
    int ret = receive_response(service, INSTALL_IPC_READY, &response, 30000);
    dump_helper_log();
    if (ret == 0) {
        service->usable = 1;
        service->pid = response.pid;
    }
    install_log("[HELPER] ready pid=%d rc=0x%08X helper_build=%.63s elapsed_ms=%lld",
                (int)service->pid, (unsigned)ret, response.build_commit,
                (long long)(install_ipc_now_ms() - began));
    return ret;
}

void install_service_close(install_service_t *service) {
    if (service->fd >= 0 && service->usable) {
        install_ipc_request_t request = {.op = INSTALL_IPC_CLOSE};
        install_ipc_response_t response;
        service->canceled = NULL;
        install_log("[HELPER] terminating AppInstUtil pid=%d", (int)service->pid);
        int ret = exchange(service, &request, &response, 1000);
        if (ret != 0) install_log("[HELPER] terminate failed pid=%d rc=0x%08X", (int)service->pid, (unsigned)ret);
    }
    if (service->fd >= 0) {
        /* EOF asks the helper to terminate its AppInstUtil session and exit.
         * Closing also detects parent death without a global PID file. */
        shutdown(service->fd, SHUT_RDWR);
        close(service->fd);
        service->fd = -1;
    }
    if (service->pid > 0) {
        int status = 0;
        for (int i = 0; i < 100; ++i) {
            pid_t ret = waitpid(service->pid, &status, WNOHANG);
            if (ret == service->pid) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    install_log("[HELPER] reaped pid=%d wait_status=0x%X exit=%d signal=%d",
                                (int)service->pid, status, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                                WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                    dump_helper_log();
                    goto reaped;
                }
                install_log("[HELPER] waitpid reported stopped pid=%d status=0x%X",
                            (int)service->pid, status);
            }
            if (ret < 0 && errno == ECHILD) {
                if (kill(service->pid, 0) < 0 && errno == ESRCH) {
                    install_log("[HELPER] reaped pid=%d (ESRCH)", (int)service->pid);
                    dump_helper_log();
                    goto reaped;
                }
            }
            if (ret < 0 && errno != EINTR && errno != ECHILD) break;
            usleep(10000);
        }
        /* A system service call may be stuck; never reuse that process. */
        install_log("[HELPER] exit timeout; sending SIGKILL pid=%d", (int)service->pid);
        int killed = kill(service->pid, SIGKILL);
        int error = killed < 0 ? errno : 0;
        pid_t waited;
        do { waited = waitpid(service->pid, &status, 0); } while (waited < 0 && errno == EINTR);
        install_log("[HELPER] forced cleanup pid=%d kill_rc=%d kill_errno=%d wait_pid=%d wait_status=0x%X signal=%d",
                    (int)service->pid, killed, error, (int)waited, status,
                    waited > 0 && WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        dump_helper_log();
    }
reaped:
    service->pid = -1;
    service->canceled = NULL;
    service->usable = 0;
}

int install_service_start(install_service_t *service, const char *uri,
                          const char *name, pkg_info_t *info,
                          install_service_canceled_fn canceled) {
    install_ipc_request_t request = {.op = INSTALL_IPC_INSTALL};
    install_ipc_response_t response = {0};
    memset(info, 0, sizeof(*info));
    if (service->fd >= 0 || service->pid > 0 || !uri || !name ||
        strlen(uri) >= sizeof(request.uri) || strlen(name) >= sizeof(request.name))
        return INSTALL_SERVICE_UNAVAILABLE;
    strcpy(request.uri, uri);
    strcpy(request.name, name);
    install_log("[HELPER] install request uri=%.180s name=%.120s", uri, name);
    int ret = open_service(service, canceled);
    if (ret == 0) ret = exchange(service, &request, &response, 120000);
    if (ret == 0) {
        *info = response.info;
        info->content_id[sizeof(info->content_id) - 1] = '\0';
        install_log("[HELPER] accepted pid=%d content_id=%s type=%d platform=%d",
                    (int)service->pid, info->content_id, info->type, info->platform);
    }
    return ret;
}

int install_service_status(install_service_t *service,
                           SceAppInstallStatusInstalled *status) {
    install_ipc_request_t request = {.op = INSTALL_IPC_STATUS};
    install_ipc_response_t response = {0};
    int ret = exchange(service, &request, &response, 10000);
    if (ret == 0) {
        *status = response.status;
        status->status[sizeof(status->status) - 1] = '\0';
    }
    int64_t now = install_ipc_now_ms();
    if (!service->have_status || ret != service->last_status_result ||
        response.status.error_info.error_code != service->last_native_error ||
        strncmp(response.status.status, service->last_status, sizeof(service->last_status)) != 0 ||
        now - service->last_status_log_ms >= 5000) {
        service->have_status = 1;
        service->last_status_result = ret;
        service->last_native_error = response.status.error_info.error_code;
        memcpy(service->last_status, response.status.status, sizeof(service->last_status));
        service->last_status_log_ms = now;
        install_log("[HELPER_STATUS] pid=%d rc=0x%08X native_ms=%u native_errno=%d status=%.16s src=%.8s downloaded=%llu total=%llu initial=%llu remain=%u promote=%u copy=%d copy_only=%d",
                    (int)service->pid, (unsigned)ret, response.native_ms, response.native_errno,
                    response.status.status, response.status.src_type,
                    (unsigned long long)response.status.downloaded_size,
                    (unsigned long long)response.status.total_size,
                    (unsigned long long)response.status.initial_chunk_size,
                    response.status.remain_time, response.status.promote_progress,
                    response.status.local_copy_percent, response.status.is_copy_only);
        if (response.status.error_info.error_code || response.status.error_info.description[0]) {
            install_log("[HELPER_STATUS] pid=%d native_error=0x%08X error_version=%d error_type=%.9s description=%.320s",
                        (int)service->pid, (unsigned)response.status.error_info.error_code,
                        response.status.error_info.version, response.status.error_info.type,
                        response.status.error_info.description);
            if (strnlen(response.status.error_info.description, sizeof(response.status.error_info.description)) > 320)
                install_log("[HELPER_STATUS] pid=%d description_cont=%.192s",
                            (int)service->pid, response.status.error_info.description + 320);
        }
    }
    return ret;
}

int install_service_shortcut(const char *title_id, const char *directory) {
    install_service_t service = INSTALL_SERVICE_INIT;
    install_ipc_request_t request = {.op = INSTALL_IPC_SHORTCUT};
    install_ipc_response_t response;
    if (!title_id || !directory || strlen(title_id) >= sizeof(request.title_id) ||
        strlen(directory) >= sizeof(request.directory)) return INSTALL_SERVICE_UNAVAILABLE;
    strcpy(request.title_id, title_id);
    strcpy(request.directory, directory);
    int ret = open_service(&service, NULL);
    if (ret == 0) ret = exchange(&service, &request, &response, 120000);
    install_service_close(&service);
    return ret;
}
