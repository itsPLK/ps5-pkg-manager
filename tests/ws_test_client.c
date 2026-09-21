/* Host WS test client: blocking masked-frame client. */

#include "ws_test_client.h"
#include "ws_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>

static int send_all_c(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int ws_client_connect(const char *host, int port, const char *path) {
    struct addrinfo hints, *res = NULL, *rp;
    char ps[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Fixed test key (RFC6455 example) -> accept must be known value. */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             path ? path : "/ws/upload", host, port, key);
    if (send_all_c(fd, req, strlen(req)) != 0) { close(fd); return -1; }
    char hs[2048];
    size_t hlen = 0;
    while (hlen < sizeof(hs) - 1) {
        ssize_t n = recv(fd, hs + hlen, sizeof(hs) - 1 - hlen, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) { close(fd); return -1; }
        hlen += (size_t)n;
        hs[hlen] = '\0';
        if (strstr(hs, "\r\n\r\n")) break;
    }
    if (!strstr(hs, "101")) { close(fd); return -1; }
    char expect[64] = {0};
    ws_direct_handshake_accept_key(key, expect, sizeof(expect));
    if (expect[0] && !strstr(hs, expect)) { close(fd); return -1; }
    return fd;
}

int ws_client_send_text(int fd, const char *s) {
    size_t n = strlen(s);
    unsigned char *fr = (unsigned char *)malloc(n + 16);
    if (!fr) return -1;
    int m = ws_direct_frame_encode_client(0x1, (const unsigned char *)s, n,
                                          fr, n + 16);
    int rc = -1;
    if (m > 0) rc = send_all_c(fd, fr, (size_t)m);
    free(fr);
    return rc;
}

int ws_client_send_binary(int fd, const void *data, unsigned long len) {
    return ws_client_send_frame(fd, 0x2, 1, data, len);
}

int ws_client_send_frame(int fd, unsigned char opcode, int fin,
                         const void *data, unsigned long len) {
    size_t extra = (len < 126) ? 0 : (len <= 65535 ? 2 : 8);
    unsigned char *fr = (unsigned char *)malloc((size_t)len + 16 + extra);
    if (!fr) return -1;
    size_t o = 0;
    fr[o++] = (unsigned char)((fin ? 0x80 : 0) | (opcode & 0x0F));
    if (len < 126) {
        fr[o++] = (unsigned char)(0x80 | len);
    } else if (len <= 65535) {
        fr[o++] = (unsigned char)(0x80 | 126);
        fr[o++] = (unsigned char)(len >> 8);
        fr[o++] = (unsigned char)len;
    } else {
        fr[o++] = (unsigned char)(0x80 | 127);
        for (int i = 7; i >= 0; i--) fr[o++] = (unsigned char)(len >> (i * 8));
    }
    unsigned char mask[4];
    mask[0] = (unsigned char)rand(); mask[1] = (unsigned char)rand();
    mask[2] = (unsigned char)rand(); mask[3] = (unsigned char)rand();
    memcpy(fr + o, mask, 4);
    o += 4;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) fr[o + i] = (p ? p[i] : 0) ^ mask[i % 4];
    int rc = send_all_c(fd, fr, o + len);
    free(fr);
    return rc;
}

int ws_client_recv_text(int fd, char *out, unsigned long max) {
    /* Heap buffer: 8 MiB must never live on (thread) stack. */
    unsigned char *rbuf = (unsigned char *)malloc(8 * 1024 * 1024 + 1024);
    if (!rbuf) return -1;
    size_t rcap = 8 * 1024 * 1024 + 1024;
    size_t rlen = 0, olen = 0;
    int rc = -1;
    out[0] = '\0';
    for (;;) {
        if (rlen >= rcap) break;
        ssize_t n = recv(fd, rbuf + rlen, rcap - rlen, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        rlen += (size_t)n;
        for (;;) {
            unsigned char op = 0;
            int fin = 0;
            size_t poff = 0, plen = 0;
            int used = ws_direct_frame_decode(rbuf, rlen, &op, &fin,
                                              &poff, &plen);
            if (used == 0) break;
            if (used < 0) goto done;
            if (op == 0x8) goto done; /* close */
            if (op == 0x9) { /* ping -> pong */
                unsigned char fr[256];
                int m = ws_direct_frame_encode_client(
                    0xA, rbuf + poff, plen > 125 ? 0 : plen, fr, sizeof(fr));
                if (m > 0) send_all_c(fd, fr, (size_t)m);
            } else if (op == 0xA) {
                /* ignore */
            } else if (op == 0x1 || op == 0x0) {
                if (olen + plen + 1 > max) goto done;
                memcpy(out + olen, rbuf + poff, plen);
                olen += plen;
                if (fin) { out[olen] = '\0'; rc = 0; goto done; }
            } else {
                goto done;
            }
            memmove(rbuf, rbuf + used, rlen - (size_t)used);
            rlen -= (size_t)used;
        }
    }
done:
    free(rbuf);
    return rc;
}

void ws_client_close(int fd) {
    unsigned char fr[16];
    int m = ws_direct_frame_encode_client(0x8, NULL, 0, fr, sizeof(fr));
    if (m > 0) send(fd, fr, (size_t)m, 0);
    close(fd);
}
