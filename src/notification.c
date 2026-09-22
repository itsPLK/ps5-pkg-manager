/*
 * PKG Manager - System Notification Dispatcher
 */

#include "notification.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#if defined(__Prospero__) || defined(PS5_BUILD)
int sceKernelSendNotificationRequest(int device, notify_request_t *request,
                                     size_t size, int unused);
#endif

void ps5_notify(const char *fmt, ...) {
    notify_request_t req;
    va_list args;

    memset(&req, 0, sizeof(req));
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, args);
    va_end(args);

#if defined(__Prospero__) || defined(PS5_BUILD)
    int result = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    if (result != 0) {
        fprintf(stderr, "[PKG Manager] Notification failed (0x%08X): %s\n",
                result, req.message);
    }
#else
    printf("[PS5 Notification] %s\n", req.message);
#endif
}
