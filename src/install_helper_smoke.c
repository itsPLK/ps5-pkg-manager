#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

static int log_line(const char *line) {
    size_t size = 0;
    while (line[size]) size++;

    int fd = open("/data/pkgmgr/helper-log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
        (void)write(fd, line, size);
        close(fd);
    }
    (void)write(STDOUT_FILENO, line, size);
    return fd >= 0 ? 0 : -1;
}

int main(void) {
    mkdir("/data/pkgmgr", 0777);
    int fd = open("/data/pkgmgr/helper-log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return 1;
    close(fd);
    log_line("MAIN ENTERED\n");

#ifdef INSTALL_HELPER_SMOKE_NET
    extern int sceNetInit(void);
    log_line("BEFORE sceNetInit\n");
    int net_result = sceNetInit();
    char net_line[80];
    snprintf(net_line, sizeof(net_line), "sceNetInit returned 0x%08X\n", (unsigned)net_result);
    log_line(net_line);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    snprintf(net_line, sizeof(net_line), "socket returned %d\n", sock);
    log_line(net_line);
    if (sock >= 0) close(sock);
#endif

#ifdef INSTALL_HELPER_SMOKE_APPINST
    extern int sceAppInstUtilInitialize(void);
    extern int sceAppInstUtilTerminate(void);
    log_line("BEFORE sceAppInstUtilInitialize\n");
    int appinst_result = sceAppInstUtilInitialize();
    char appinst_line[96];
    snprintf(appinst_line, sizeof(appinst_line),
             "sceAppInstUtilInitialize returned 0x%08X\n", (unsigned)appinst_result);
    log_line(appinst_line);
    if (appinst_result == 0) {
        log_line("BEFORE sceAppInstUtilTerminate\n");
        int terminate_result = sceAppInstUtilTerminate();
        snprintf(appinst_line, sizeof(appinst_line),
                 "sceAppInstUtilTerminate returned 0x%08X\n", (unsigned)terminate_result);
        log_line(appinst_line);
    }
#endif

    log_line("SMOKE COMPLETE\n");
    return 0;
}
