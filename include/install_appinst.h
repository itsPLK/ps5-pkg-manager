#ifndef INSTALL_APPINST_H
#define INSTALL_APPINST_H

#include "install_service.h"

/* Native pointer-bearing ABI. Only the helper constructs these structures;
 * they are never sent over IPC. */
typedef struct {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
} pkg_metadata_t;

typedef struct {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][48];
    unsigned char unknown[6480];
} playgo_info_t;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilInstallByPackage(const pkg_metadata_t *, pkg_info_t *, playgo_info_t *);
int sceAppInstUtilGetInstallStatus(const char *, SceAppInstallStatusInstalled *);
int sceAppInstUtilAppInstallAll(void *);

#endif
