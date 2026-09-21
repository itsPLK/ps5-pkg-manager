#ifndef WS_TEST_CLIENT_H
#define WS_TEST_CLIENT_H

/* Host test helper: minimal blocking WebSocket client (masked frames).
 * Used by test_ws_upload / test_direct_install_e2e / ws_push_sim.
 * No dependency on the server listener internals beyond ws_upload.h codec.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Connect + handshake GET path (e.g. "/ws/upload"). Returns fd or -1. */
int ws_client_connect(const char *host, int port, const char *path);

/* Send one text / binary message (single frame, masked). 0 on ok. */
int ws_client_send_text(int fd, const char *s);
int ws_client_send_binary(int fd, const void *data, unsigned long len);

/* Send one raw frame with explicit opcode/FIN (for fragmentation tests).
 * opcode: 0x0 continuation, 0x1 text, 0x2 binary. 0 on ok. */
int ws_client_send_frame(int fd, unsigned char opcode, int fin,
                         const void *data, unsigned long len);

/* Receive one text message (reassembles fragments, answers pings).
 * Returns 0 on ok with NUL-terminated payload in out. -1 on error/close. */
int ws_client_recv_text(int fd, char *out, unsigned long max);

/* Clean close handshake (best effort) + close(fd). */
void ws_client_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* WS_TEST_CLIENT_H */
