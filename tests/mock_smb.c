#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

struct smb2_context {
    char server[128];
    char share[128];
    int credits;
    uint16_t dialect;
};

struct smb2dir {
    DIR *d;
};

struct smb2_stat_64 {
    uint64_t smb2_size;
    uint32_t smb2_mtime;
    int smb2_type;
};

struct smb2dirent {
    char name[256];
    struct smb2_stat_64 st;
};

struct smb2fh {
    int fd;
};

#define SMB2_TYPE_FILE 1
#define SMB2_TYPE_DIRECTORY 2

struct smb2_context *smb2_init_context(void) {
    struct smb2_context *ctx = (struct smb2_context *)calloc(1, sizeof(struct smb2_context));
    if (ctx) {
        ctx->credits = 128;
        ctx->dialect = 0x0300;
    }
    return ctx;
}

void smb2_destroy_context(struct smb2_context *smb2) {
    if (smb2) free(smb2);
}

void smb2_set_timeout(struct smb2_context *smb2, int seconds) { (void)smb2; (void)seconds; }
void smb2_set_security_mode(struct smb2_context *smb2, uint16_t security_mode) { (void)smb2; (void)security_mode; }
void smb2_set_user(struct smb2_context *smb2, const char *user) { (void)smb2; (void)user; }
void smb2_set_password(struct smb2_context *smb2, const char *password) { (void)smb2; (void)password; }
void smb2_set_domain(struct smb2_context *smb2, const char *domain) { (void)smb2; (void)domain; }

int smb2_connect_share(struct smb2_context *smb2, const char *server, const char *share, const char *user) {
    (void)user;
    if (!smb2) return -1;
    strncpy(smb2->server, server ? server : "", sizeof(smb2->server) - 1);
    strncpy(smb2->share, share ? share : "", sizeof(smb2->share) - 1);
    return 0;
}

const char *smb2_get_error(struct smb2_context *smb2) { (void)smb2; return "mock smb error"; }
int smb2_get_nterror(struct smb2_context *smb2) { (void)smb2; return 0; }
const char *nterror_to_str(uint32_t status) { (void)status; return "STATUS_SUCCESS"; }
typedef void (*smb2_error_cb)(struct smb2_context *smb2, const char *error_string);
void smb2_register_error_callback(struct smb2_context *smb2, smb2_error_cb error_cb) { (void)smb2; (void)error_cb; }

struct smb2dir *smb2_opendir(struct smb2_context *smb2, const char *path) {
    if (!smb2) return NULL;
    char local_path[1024];
    while (path && *path == '/') path++;
    snprintf(local_path, sizeof(local_path), "/tmp/mock_smb/%s", path ? path : "");
    DIR *d = opendir(local_path);
    if (!d) return NULL;
    struct smb2dir *dir = (struct smb2dir *)calloc(1, sizeof(struct smb2dir));
    if (!dir) {
        closedir(d);
        return NULL;
    }
    dir->d = d;
    return dir;
}

struct smb2dirent *smb2_readdir(struct smb2_context *smb2, struct smb2dir *dir) {
    (void)smb2;
    if (!dir || !dir->d) return NULL;
    struct dirent *de = readdir(dir->d);
    if (!de) return NULL;
    static struct smb2dirent ent;
    memset(&ent, 0, sizeof(ent));
    strncpy(ent.name, de->d_name, sizeof(ent.name) - 1);
    ent.st.smb2_type = (de->d_type == DT_DIR) ? SMB2_TYPE_DIRECTORY : SMB2_TYPE_FILE;
    return &ent;
}

void smb2_closedir(struct smb2_context *smb2, struct smb2dir *dir) {
    (void)smb2;
    if (!dir) return;
    if (dir->d) closedir(dir->d);
    free(dir);
}

struct smb2fh *smb2_open(struct smb2_context *smb2, const char *path, int flags) {
    if (!smb2 || !path) return NULL;
    char local_path[1024];
    while (*path == '/') path++;
    snprintf(local_path, sizeof(local_path), "/tmp/mock_smb/%s", path);

    int fd = open(local_path, flags, 0644);
    if (fd < 0) return NULL;

    struct smb2fh *fh = (struct smb2fh *)calloc(1, sizeof(struct smb2fh));
    if (!fh) {
        close(fd);
        return NULL;
    }
    fh->fd = fd;
    return fh;
}

int smb2_close(struct smb2_context *smb2, struct smb2fh *fh) {
    (void)smb2;
    if (!fh) return 0;
    if (fh->fd >= 0) close(fh->fd);
    free(fh);
    return 0;
}

int smb2_unlink(struct smb2_context *smb2, const char *path) {
    (void)smb2;
    if (!path) return -1;
    char local_path[1024];
    while (*path == '/') path++;
    snprintf(local_path, sizeof(local_path), "/tmp/mock_smb/%s", path);
    return unlink(local_path);
}

int smb2_fstat(struct smb2_context *smb2, struct smb2fh *fh, struct smb2_stat_64 *st) {
    (void)smb2;
    if (!fh || !st) return -1;
    struct stat s;
    if (fstat(fh->fd, &s) != 0) return -1;
    st->smb2_size = (uint64_t)s.st_size;
    st->smb2_mtime = (uint32_t)s.st_mtime;
    st->smb2_type = S_ISDIR(s.st_mode) ? SMB2_TYPE_DIRECTORY : SMB2_TYPE_FILE;
    return 0;
}

long smb2_pread(struct smb2_context *smb2, struct smb2fh *fh, unsigned char *buf, unsigned long count, unsigned long offset) {
    (void)smb2;
    if (!fh || !buf) return -1;
    return (long)pread(fh->fd, buf, count, (off_t)offset);
}

long smb2_pwrite(struct smb2_context *smb2, struct smb2fh *fh, const unsigned char *buf, unsigned long count, unsigned long offset) {
    (void)smb2;
    if (!fh || !buf) return -1;
    return (long)pwrite(fh->fd, buf, count, (off_t)offset);
}

int smb2_stat(struct smb2_context *smb2, const char *path, struct smb2_stat_64 *st) {
    (void)smb2;
    if (!path || !st) return -1;
    char local_path[1024];
    while (*path == '/') path++;
    snprintf(local_path, sizeof(local_path), "/tmp/mock_smb/%s", path);
    struct stat s;
    if (stat(local_path, &s) != 0) return -1;
    st->smb2_size = (uint64_t)s.st_size;
    st->smb2_mtime = (uint32_t)s.st_mtime;
    st->smb2_type = S_ISDIR(s.st_mode) ? SMB2_TYPE_DIRECTORY : SMB2_TYPE_FILE;
    return 0;
}

int smb2_mkdir(struct smb2_context *smb2, const char *path) { (void)smb2; (void)path; return -1; }
int smb2_rmdir(struct smb2_context *smb2, const char *path) { (void)smb2; (void)path; return -1; }

uint32_t smb2_get_max_read_size(struct smb2_context *smb2) {
    (void)smb2;
    return 1048576;
}

int smb2_get_fd(struct smb2_context *smb2) {
    (void)smb2;
    return 0;
}

int smb2_which_events(struct smb2_context *smb2) {
    (void)smb2;
    return 1;
}

int smb2_service(struct smb2_context *smb2, int revents) {
    (void)smb2;
    (void)revents;
    return 0;
}

typedef void (*smb2_command_cb_t)(struct smb2_context *smb2, int status,
                                  void *command_data, void *private_data);

int smb2_pread_async(struct smb2_context *smb2, struct smb2fh *fh,
                     uint8_t *buf, uint32_t count, uint64_t offset,
                     smb2_command_cb_t cb, void *cb_data) {
    if (!fh || !buf) return -1;
    ssize_t n = pread(fh->fd, buf, count, (off_t)offset);
    if (cb) {
        cb(smb2, (int)n, NULL, cb_data);
    }
    return 0;
}
