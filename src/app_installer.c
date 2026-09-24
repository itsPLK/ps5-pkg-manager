/*
 * PS5 App Installer for PKG Manager
 * Installs the PKG Manager shortcut to the PS5 home screen (Media tab)
 * Based on the implementation in ps5-payload-manager.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_installer.h"
#include "notification.h"
#include "assets_param_json.h"
#include "assets_icon0_png.h"

#include "install_service.h"


static int install_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, size, f) != size) {
        int write_errno = errno ? errno : EIO;
        fclose(f);
        errno = write_errno;
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int install_app(const char *title_id, const char *dir) {
#if defined(__Prospero__) || defined(PS5_BUILD)
    return install_service_shortcut(title_id, dir);
#else
    (void)title_id;
    (void)dir;
    printf("[APP_INSTALLER] Mock install_app(%s, %s)\n", title_id, dir);
    return 0;
#endif
}

static int needs_update(const char *path, const uint8_t *expected_data, size_t expected_size) {
    struct stat st;
    if (stat(path, &st) != 0) return 1;
    if ((size_t)st.st_size != expected_size) return 1;

    FILE *f = fopen(path, "rb");
    if (!f) return 1;

    uint8_t *buf = (uint8_t *)malloc(expected_size);
    if (!buf) {
        fclose(f);
        return 1;
    }

    if (fread(buf, 1, expected_size, f) != expected_size) {
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    int mismatch = memcmp(buf, expected_data, expected_size);
    free(buf);
    return mismatch != 0;
}

static int do_install(int is_update) {
    const char *title_id = PKGMGR_TITLE_ID;
    char base_dir[256];
    char sce_sys_dir[256];
    char param_path[256];
    char icon_path[256];

    snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
    snprintf(sce_sys_dir, sizeof(sce_sys_dir), "/user/app/%s/sce_sys", title_id);
    snprintf(param_path, sizeof(param_path), "/user/app/%s/sce_sys/param.json", title_id);
    snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png", title_id);

    if (is_update) {
        printf("[APP_INSTALLER] Updating PKG Manager launcher (%s)...\n", title_id);
        ps5_notify("Updating PKG Manager Shortcut...");
    } else {
        printf("[APP_INSTALLER] Installing PKG Manager launcher (%s)...\n", title_id);
        ps5_notify("Installing PKG Manager Shortcut...");
    }

    /* Registration uses its own short-lived helper and cannot consume a
     * package install's AppInstUtil/PlayGo session. */

    if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) {
        int error = errno;
        printf("[APP_INSTALLER] Failed to create app dir %s: %s\n", base_dir, strerror(error));
        ps5_notify("PKG Manager shortcut: app directory failed (errno %d)", error);
        return -1;
    }

    if (mkdir(sce_sys_dir, 0755) != 0 && errno != EEXIST) {
        int error = errno;
        printf("[APP_INSTALLER] Failed to create sce_sys dir %s: %s\n", sce_sys_dir, strerror(error));
        ps5_notify("PKG Manager shortcut: sce_sys directory failed (errno %d)", error);
        return -1;
    }

    if (install_file(param_path, assets_param_json, assets_param_json_len) != 0) {
        int error = errno;
        printf("[APP_INSTALLER] Failed to write %s\n", param_path);
        ps5_notify("PKG Manager shortcut: param.json write failed (errno %d)", error);
        return -1;
    }

    if (install_file(icon_path, assets_icon0_png, assets_icon0_png_len) != 0) {
        int error = errno;
        printf("[APP_INSTALLER] Failed to write %s\n", icon_path);
        ps5_notify("PKG Manager shortcut: icon0.png write failed (errno %d)", error);
        return -1;
    }

    int inst_err = install_app(title_id, "/user/app/");
    if (inst_err != 0) {
        printf("[APP_INSTALLER] install_app error: 0x%08X\n", inst_err);
        ps5_notify("PKG Manager shortcut: install returned 0x%08X", inst_err);
        return -1;
    }

    printf("[APP_INSTALLER] PKG Manager launcher installed successfully (%s).\n", title_id);
    ps5_notify("PKG Manager Shortcut Ready!");

    return 0;
}

int app_installer_install_if_needed(void) {
    const char *title_id = PKGMGR_TITLE_ID;
    char base_dir[256];
    char param_path[256];
    char icon_path[256];

    snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
    snprintf(param_path, sizeof(param_path), "/user/app/%s/sce_sys/param.json", title_id);
    snprintf(icon_path, sizeof(icon_path), "/user/app/%s/sce_sys/icon0.png", title_id);

    struct stat st;
    int is_existing = (stat(base_dir, &st) == 0);
    int update_needed = 0;

    if (!is_existing) {
        update_needed = 1;
    } else {
        if (needs_update(param_path, assets_param_json, assets_param_json_len)) {
            update_needed = 1;
        }
        if (needs_update(icon_path, assets_icon0_png, assets_icon0_png_len)) {
            update_needed = 1;
        }
    }

    if (!update_needed) {
        printf("[APP_INSTALLER] PKG Manager launcher (%s) is already up to date.\n", title_id);
        return 0;
    }

    return do_install(is_existing);
}

int app_installer_force_install(void) {
    const char *title_id = PKGMGR_TITLE_ID;
    char base_dir[256];
    snprintf(base_dir, sizeof(base_dir), "/user/app/%s", title_id);
    struct stat st;
    int is_existing = (stat(base_dir, &st) == 0);
    return do_install(is_existing);
}
