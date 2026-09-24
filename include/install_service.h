#ifndef INSTALL_SERVICE_H
#define INSTALL_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* AppInstUtil ABI, shared by the daemon and its embedded helper. */
typedef struct {
    char content_id[48];
    int type;
    int platform;
} pkg_info_t;

typedef struct {
    int32_t error_code;
    int32_t version;
    char description[512];
    char type[9];
} SceAppInstallErrorInfo;

typedef struct {
    char status[16];
    char src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size;
    uint64_t initial_chunk_size;
    uint64_t total_size;
    uint32_t promote_progress;
    SceAppInstallErrorInfo error_info;
    int32_t local_copy_percent;
    bool is_copy_only;
} SceAppInstallStatusInstalled;

/* Local failures are distinct from errors returned by AppInstUtil. */
#define INSTALL_SERVICE_UNAVAILABLE (-2001)
#define INSTALL_SERVICE_DISCONNECTED (-2002)
#define INSTALL_SERVICE_CANCELED (-2003)
#define INSTALL_SERVICE_TIMEOUT (-2004)

typedef int (*install_service_canceled_fn)(void);
typedef struct {
    pid_t pid;
    int fd;
    install_service_canceled_fn canceled;
    int usable;
    int have_status;
    int last_status_result;
    int32_t last_native_error;
    char last_status[16];
    int64_t last_status_log_ms;
} install_service_t;

#define INSTALL_SERVICE_INIT { .pid = -1, .fd = -1, .canceled = NULL }

/* Each start launches a fresh helper. The caller owns the session and must
 * close it on every exit, including a failed start, before starting another. */
int install_service_start(install_service_t *service, const char *uri,
                          const char *name, pkg_info_t *info,
                          install_service_canceled_fn canceled);
int install_service_status(install_service_t *service,
                           SceAppInstallStatusInstalled *status);
void install_service_close(install_service_t *service);
int install_service_shortcut(const char *title_id, const char *directory);

#endif
