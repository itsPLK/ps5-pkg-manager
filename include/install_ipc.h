#ifndef INSTALL_IPC_H
#define INSTALL_IPC_H

#include <stddef.h>
#include "install_service.h"

/* Same build and ABI at both ends; no pointers cross the socket. */
#define INSTALL_IPC_MAGIC 0x504b4749u
#define INSTALL_IPC_VERSION 1u
#define INSTALL_IPC_PORT 18843
#define ELFLDR_PORT 9021
/* Avoid descriptors used internally by the system image/libc at startup. */
#define INSTALL_IPC_FD 198

enum install_ipc_op {
    INSTALL_IPC_READY = 0,
    INSTALL_IPC_INSTALL,
    INSTALL_IPC_STATUS,
    INSTALL_IPC_SHORTCUT,
    INSTALL_IPC_CLOSE
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t op;
    char uri[1024];
    char name[320];
    char title_id[32];
    char directory[64];
} install_ipc_request_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t op;
    int32_t result;
    int32_t pid;
    uint32_t native_ms;
    int32_t native_errno;
    char build_commit[64];
    pkg_info_t info;
    SceAppInstallStatusInstalled status;
} install_ipc_response_t;

int install_ipc_transfer(int fd, void *buffer, size_t size, int sending,
                         int timeout_ms, install_service_canceled_fn canceled);
int install_helper_serve(int fd);
int64_t install_ipc_now_ms(void);

#endif
