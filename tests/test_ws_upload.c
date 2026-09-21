/*
 * Host test: ws_direct transport + live RAM backend (NEW modules only).
 * Never touches installer.c worker logic / stream_server.c / pkg_parser.c
 * file paths. Byte storage assertions read back through ws_live_read:
 * nothing is stored on disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "ws_upload.h"
#include "ws_stream.h"
#include "ws_test_client.h"

static void reset(void) {
    ws_direct_reset_for_tests();
    ws_live_set_timeout_sec(10);
    ws_direct_listener_stop();
}

static void fill_pat(unsigned char *b, size_t n, unsigned off) {
    for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(((off + i) * 31 + 7) & 0xFF);
}

static void expect_pat(const unsigned char *b, size_t n, unsigned off) {
    for (size_t i = 0; i < n; i++) {
        unsigned char want = (unsigned char)(((off + i) * 31 + 7) & 0xFF);
        assert(b[i] == want);
    }
}

static void test_rfc_vector(void) {
    /* RFC6455 §1.3 example: key -> accept must match the spec value. */
    char accept[64] = {0};
    assert(ws_direct_handshake_accept_key("dGhlIHNhbXBsZSBub25jZQ==",
                                          accept, sizeof(accept)) == 0);
    assert(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    printf("  rfc-vector ok (%s)\n", accept);
}

static void test_frame_roundtrip(void) {
    /* Server frame (unmasked) decodes; client frame (masked) unmasks. */
    unsigned char enc[256];
    unsigned char work[1024];
    const char *msg = "hello-ws";
    int n = ws_direct_frame_encode_server(0x1, (const unsigned char *)msg,
                                          strlen(msg), enc, sizeof(enc));
    assert(n > 0);
    memcpy(work, enc, (size_t)n);
    unsigned char op = 0;
    int fin = 0;
    size_t off = 0, len = 0;
    assert(ws_direct_frame_decode(work, (size_t)n, &op, &fin, &off, &len) == n);
    assert(op == 0x1 && fin == 1 && len == strlen(msg));
    assert(memcmp(work + off, msg, len) == 0);

    /* Masked client frame with 2-byte extended length. */
    unsigned char big[300];
    memset(big, 0xAB, sizeof(big));
    unsigned char cenc[512];
    int m = ws_direct_frame_encode_client(0x2, big, sizeof(big), cenc, sizeof(cenc));
    assert(m > 0);
    assert(cenc[1] == (unsigned char)(0x80 | 126)); /* masked + 126 */
    memcpy(work, cenc, (size_t)m);
    assert(ws_direct_frame_decode(work, (size_t)m, &op, &fin, &off, &len) == m);
    assert(op == 0x2 && len == sizeof(big));
    assert(memcmp(work + off, big, sizeof(big)) == 0);

    /* Partial decode: header only -> need more (0). */
    memcpy(work, cenc, 2);
    assert(ws_direct_frame_decode(work, 2, &op, &fin, &off, &len) == 0);
    printf("  frame-roundtrip ok\n");
}

static void write_pattern(uint64_t total) {
    /* Segment-quantized deterministic pattern. */
    unsigned char chunk[WS_LIVE_SEG_SIZE];
    uint64_t nsegs = (total + WS_LIVE_SEG_SIZE - 1) / WS_LIVE_SEG_SIZE;
    for (uint64_t s = 0; s < nsegs; s++) {
        size_t n = WS_LIVE_SEG_SIZE;
        if (s * WS_LIVE_SEG_SIZE + n > total) n = (size_t)(total - s * WS_LIVE_SEG_SIZE);
        fill_pat(chunk, n, (unsigned)(s * WS_LIVE_SEG_SIZE));
        assert(ws_direct_write_chunk(s * WS_LIVE_SEG_SIZE, chunk, n, NULL) == 0);
    }
}

static void test_live_basic(void) {
    reset();
    char sid[64] = {0};
    assert(ws_direct_init_session("game.pkg", 200000, sid, sizeof(sid)) == 0);
    assert(sid[0] != '\0' && ws_direct_session_active());
    /* Busy: different file rejected. */
    char sid2[64] = {0};
    assert(ws_direct_init_session("other.pkg", 10, sid2, sizeof(sid2)) == -2);
    /* Same file re-init is idempotent resume. */
    char sid3[64] = {0};
    assert(ws_direct_init_session("game.pkg", 200000, sid3, sizeof(sid3)) == 0);
    assert(strcmp(sid, sid3) == 0);
    /* Segment protocol: unaligned writes rejected (no offset cursor). */
    unsigned char b[16] = {0};
    assert(ws_direct_write_chunk(16, b, sizeof(b), NULL) != 0);
    /* Oversize rejected. */
    assert(ws_direct_write_chunk(0, b, 200001, NULL) != 0);
    /* Finish before complete rejected. */
    char uri[640] = {0};
    assert(ws_direct_finish_session(uri, sizeof(uri)) != 0);
    write_pattern(200000);
    assert(ws_direct_finish_session(uri, sizeof(uri)) == 0);
    assert(strncmp(uri, "live:", 5) == 0);
    /* Live session stays active after finish so late pullers can read. */
    assert(ws_direct_session_active() && ws_live_session_active());
    char st[1024];
    assert(ws_direct_get_status(st, sizeof(st)) == 0);
    assert(strstr(st, "\"received\":200000") != NULL);
    assert(strstr(st, "\"complete\":true") != NULL);
    unsigned char *all = malloc(200000);
    assert(all);
    assert(ws_live_read(0, all, 200000) == 200000);
    expect_pat(all, 200000, 0);
    free(all);
    printf("  live-basic ok (%s)\n", uri);
    ws_direct_reset_for_tests();
    assert(!ws_direct_session_active());
}

static void test_exact_boundary_and_tail(void) {
    reset();
    /* Exact 1 MiB + 1 byte tail exercises extended-length + tail frames. */
    uint64_t total = 1024 * 1024 + 1;
    char sid[64] = {0};
    assert(ws_direct_init_session("edge.pkg", total, sid, sizeof(sid)) == 0);
    unsigned char *big = (unsigned char *)malloc(1024 * 1024);
    assert(big);
    memset(big, 0x5A, 1024 * 1024);
    assert(ws_direct_write_chunk(0, big, 1024 * 1024, NULL) == 0);
    unsigned char tail = 0xE7;
    assert(ws_direct_write_chunk(1024 * 1024, &tail, 1, NULL) == 0);
    char uri[640] = {0};
    assert(ws_direct_finish_session(uri, sizeof(uri)) == 0);
    unsigned char got = 0;
    assert(ws_live_read(1024 * 1024, &got, 1) == 1 && got == 0xE7);
    free(big);
    printf("  boundary-tail ok\n");
    ws_direct_reset_for_tests();
}

static void test_resume_and_cancel(void) {
    reset();
    uint64_t total = 2ULL * WS_LIVE_SEG_SIZE + 100;
    char sid[64] = {0};
    assert(ws_direct_init_session("resume.pkg", total, sid, sizeof(sid)) == 0);
    static unsigned char b[WS_LIVE_SEG_SIZE];
    memset(b, 0x11, sizeof(b));
    assert(ws_direct_write_chunk(0, b, sizeof(b), NULL) == 0);
    /* Same-session re-init is idempotent (same id). */
    char sid2[64] = {0};
    assert(ws_direct_init_session("resume.pkg", total, sid2, sizeof(sid2)) == 0);
    assert(strcmp(sid, sid2) == 0);
    char st[1024];
    assert(ws_direct_get_status(st, sizeof(st)) == 0);
    char want[64];
    snprintf(want, sizeof(want), "\"received\":%u", WS_LIVE_SEG_SIZE);
    assert(strstr(st, want) != NULL);
    /* Cancel tears the session down; nothing was ever stored on disk. */
    ws_direct_cancel_session();
    assert(!ws_direct_session_active());
    assert(!ws_live_session_active());
    assert(ws_direct_get_status(st, sizeof(st)) == 0);
    assert(strstr(st, "\"active\":false") != NULL);
    /* Fresh init after cancel works. */
    assert(ws_direct_init_session("resume.pkg", total, sid2, sizeof(sid2)) == 0);
    ws_direct_cancel_session();
    printf("  resume-cancel ok\n");
    ws_direct_reset_for_tests();
}

static void test_live_socket_roundtrip(void) {
    reset();
    const int port = 18846;
    setenv("WS_DIRECT_PORT", "18846", 1);
    assert(ws_direct_listener_start(port) == 0);
    assert(ws_direct_listener_running());
    int fd = ws_client_connect("127.0.0.1", port, "/ws/upload");
    assert(fd >= 0);
    assert(ws_client_send_text(fd, "{\"op\":\"init\",\"filename\":\"sock.pkg\",\"total\":3000}") == 0);
    char rep[1024] = {0};
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"ready\"") != NULL);
    assert(strstr(rep, "\"offset\":0") != NULL);
    unsigned char chunk[3000];
    memset(chunk, 0x33, sizeof(chunk));
    assert(ws_client_send_text(fd, "{\"op\":\"seg\",\"seg\":0}") == 0);
    assert(ws_client_send_binary(fd, chunk, sizeof(chunk)) == 0);
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"ack\"") != NULL);
    assert(strstr(rep, "\"seg\":0") != NULL);
    assert(ws_client_send_text(fd, "{\"op\":\"finish\"}") == 0);
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"complete\"") != NULL);
    assert(strstr(rep, "live:") != NULL);
    ws_client_close(fd);
    /* Bytes landed in RAM, readable post-finish. */
    unsigned char back[3000];
    assert(ws_live_read(0, back, sizeof(back)) == (long)sizeof(back));
    for (size_t i = 0; i < sizeof(back); i++) assert(back[i] == 0x33);
    /* Bad path must 400, not upgrade. */
    int bad = ws_client_connect("127.0.0.1", port, "/nope");
    assert(bad < 0);
    ws_direct_listener_stop();
    unsetenv("WS_DIRECT_PORT");
    printf("  socket-roundtrip ok\n");
    ws_direct_reset_for_tests();
}

/* Browsers fragment large binary messages (0x2 start + 0x0 continuations);
 * continuations must reach binary reassembly, and one ack covers the whole
 * logical message. */
static void test_fragmented_messages(void) {
    reset();
    const int port = 18849;
    setenv("WS_DIRECT_PORT", "18849", 1);
    assert(ws_direct_listener_start(port) == 0);
    static unsigned char f1[130933], f2[130933], f3[38134];
    memset(f1, 0x41, sizeof(f1));
    memset(f2, 0x42, sizeof(f2));
    memset(f3, 0x43, sizeof(f3));
    const uint64_t total = sizeof(f1) + sizeof(f2) + sizeof(f3);

    int fd = ws_client_connect("127.0.0.1", port, "/ws/upload");
    assert(fd >= 0);
    char init[256];
    snprintf(init, sizeof(init),
             "{\"op\":\"init\",\"filename\":\"frag.pkg\",\"total\":%llu}",
             (unsigned long long)total);
    assert(ws_client_send_text(fd, init) == 0);
    char rep[1024] = {0};
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"ready\"") != NULL);

    char segmsg[64];
    snprintf(segmsg, sizeof(segmsg), "{\"op\":\"seg\",\"seg\":0}");
    assert(ws_client_send_text(fd, segmsg) == 0);
    assert(ws_client_send_frame(fd, 0x2, 0, f1, sizeof(f1)) == 0);
    assert(ws_client_send_frame(fd, 0x0, 0, f2, sizeof(f2)) == 0);
    assert(ws_client_send_frame(fd, 0x0, 1, f3, sizeof(f3)) == 0);
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"ack\"") != NULL);
    assert(strstr(rep, "\"seg\":0") != NULL);

    assert(ws_client_send_text(fd, "{\"op\":\"finish\"}") == 0);
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"complete\"") != NULL);
    ws_client_close(fd);

    unsigned char probe[130933];
    assert(ws_live_read(0, probe, sizeof(f1)) == (long)sizeof(f1));
    assert(memcmp(probe, f1, sizeof(f1)) == 0);
    assert(ws_live_read(sizeof(f1), probe, sizeof(f2)) == (long)sizeof(f2));
    assert(memcmp(probe, f2, sizeof(f2)) == 0);
    unsigned char tail[38134];
    assert(ws_live_read(sizeof(f1) + sizeof(f2), tail, sizeof(tail)) == (long)sizeof(tail));
    assert(memcmp(tail, f3, sizeof(tail)) == 0);

    ws_direct_listener_stop();
    unsetenv("WS_DIRECT_PORT");
    printf("  fragmented-messages ok\n");
    ws_direct_reset_for_tests();
}

/* Push one whole segment over the socket; expects an ack or busy reply
 * naming seg. Returns 1 on ack, 0 on busy, -1 on transport failure. */
static int push_seg_expect(int fd, uint64_t seg, unsigned char pat, char *rep, size_t rep_max) {
    static unsigned char chunk[WS_LIVE_SEG_SIZE];
    memset(chunk, pat, sizeof(chunk));
    char segmsg[64];
    snprintf(segmsg, sizeof(segmsg), "{\"op\":\"seg\",\"seg\":%llu}",
             (unsigned long long)seg);
    if (ws_client_send_text(fd, segmsg) != 0) return -1;
    if (ws_client_send_binary(fd, chunk, sizeof(chunk)) != 0) return -1;
    if (ws_client_recv_text(fd, rep, rep_max) != 0) return -1;
    if (strstr(rep, "\"ack\"") != NULL) return 1;
    if (strstr(rep, "\"busy\"") != NULL) return 0;
    return -1;
}

/* Ring-busy over a real socket: with a 4-slot ring and an 8-seg file the
 * 5th push finds no evictable slot (sequential mode: peak_seg == 0,
 * nothing served, nothing waited-on) and must get "busy" -- the worker
 * stays responsive instead of wedging. Serving one read frees a
 * served-past slot, so the retry lands with "ack". */
static void test_socket_busy_retry(void) {
    reset();
    const int port = 18850;
    setenv("WS_LIVE_SLOTS", "4", 1);
    setenv("WS_DIRECT_PORT", "18850", 1);
    assert(ws_direct_listener_start(port) == 0);
    const uint64_t total = 8ULL * WS_LIVE_SEG_SIZE;
    int fd = ws_client_connect("127.0.0.1", port, "/ws/upload");
    assert(fd >= 0);
    char init[256];
    snprintf(init, sizeof(init),
             "{\"op\":\"init\",\"filename\":\"busy.pkg\",\"total\":%llu}",
             (unsigned long long)total);
    char rep[1024] = {0};
    assert(ws_client_send_text(fd, init) == 0);
    assert(ws_client_recv_text(fd, rep, sizeof(rep)) == 0);
    assert(strstr(rep, "\"ready\"") != NULL);
    for (uint64_t s = 0; s < 4; s++) {
        assert(push_seg_expect(fd, s, (unsigned char)(0xC0 + s), rep, sizeof(rep)) == 1);
    }
    assert(push_seg_expect(fd, 4, 0xC4, rep, sizeof(rep)) == 0); /* busy */
    assert(strstr(rep, "\"seg\":4") != NULL);
    /* Serve seg 1: it becomes served-past, freeing a victim slot. */
    static unsigned char rbuf[WS_LIVE_SEG_SIZE];
    assert(ws_live_read(WS_LIVE_SEG_SIZE, rbuf, sizeof(rbuf)) == (long)sizeof(rbuf));
    assert(rbuf[0] == (unsigned char)0xC1);
    assert(push_seg_expect(fd, 4, 0xC4, rep, sizeof(rep)) == 1); /* ack now */
    ws_client_close(fd);
    ws_direct_listener_stop();
    unsetenv("WS_LIVE_SLOTS");
    unsetenv("WS_DIRECT_PORT");
    printf("  socket-busy-retry ok\n");
    ws_direct_reset_for_tests();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("==============================================\n");
    printf(">>> RUNNING WS_DIRECT UPLOAD TEST <<<\n");
    printf("==============================================\n");
    srand(12345);
    test_rfc_vector();
    test_frame_roundtrip();
    test_live_basic();
    test_exact_boundary_and_tail();
    test_resume_and_cancel();
    test_live_socket_roundtrip();
    test_fragmented_messages();
    test_socket_busy_retry();
    ws_direct_listener_stop();
    printf("\n>>> ALL WS_DIRECT TESTS PASSED! <<<\n");
    return 0;
}
