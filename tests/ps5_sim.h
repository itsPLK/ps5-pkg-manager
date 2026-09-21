/*
 * PS5 Installer Stream Simulator — shared definitions.
 *
 * Models the request pattern that the PlayStation 5 background package
 * installer (libSceAppInstUtil / ShellCore downloader) sends to the PKG
 * Manager virtual stream server (stream_server.c, TCP :8845).
 *
 * The pattern, reverse-engineered from the captured logs in
 * .for_reference/stream_debug/, has three phases:
 *
 *   1. Header acquisition — a burst of short-lived connections, each
 *      re-reading the first 64 KiB (Range: bytes=0-65535) so the installer
 *      can parse / re-validate the package header.
 *   2. Sidecar CRC probe  — a single Range-less GET of
 *      "<content_id>.crc" that the installer expects to 404 when we have no
 *      companion file; a 206 of PKG bytes here would poison chunk checks.
 *   3. Bulk transfer      — two parallel long-lived connections serving
 *      contiguous 16 MiB byte-ranges across [65536, end) in an A/B ping-pong
 *      until the whole package has been delivered.
 *
 * Every request carries the query the console appends:
 *   ?product=0287&serverIpAddr=127.0.0.1&r=00000000
 *
 * This header is the pure-client half of the model; the end-to-end test
 * (test_stream_sim.c) pairs it against the real stream_server.c. The CLI
 * (tools/ps5_installer_sim.c) points the same client at any live server,
 * which is exactly the shape a future websocket / remote install method
 * needs.
 */

#ifndef PS5_SIM_H
#define PS5_SIM_H

#include <stdio.h>
#include <stdint.h>

/* Observed request query string (ShellCore appends these params). */
#define PS5_SIM_DEFAULT_QUERY "?product=0287&serverIpAddr=127.0.0.1&r=00000000"

/* First-chunk header read size the console re-fetches repeatedly. */
#define PS5_SIM_HEADER_END (64 * 1024)

/* Bulk chunk size observed in every capture (16 MiB). */
#define PS5_SIM_DEFAULT_CHUNK (16 * 1024 * 1024)

/* Tunables — all default to values seen in the reference captures. */
typedef struct {
    const char *host;          /* e.g. "127.0.0.1" or real PS5 IP later      */
    int         port;          /* e.g. 8845                                   */
    const char *path;          /* /stream/install/package-<ts>-<n>.pkg        */
    const char *content_id;    /* for the .crc sidecar probe; NULL/"" = skip   */
    const char *query;         /* query string; NULL => default                 */

    int    header_repeats;     /* header-phase connections (observed 2..7)      */
    int    reqs_per_header;    /* requests per header connection (observed 1..2)*/
    int    parallel;           /* bulk parallel connections (observed 1..2)     */
    uint64_t chunk_size;       /* bulk chunk size                               */
    uint64_t total_size;       /* total pkg size; 0 => learn from server         */

    int    check_crc;          /* 1 => probe <content_id>.crc, expect 404       */
} ps5_sim_target_t;

/* Per-run accounting so callers (and the replay log) can see what happened. */
typedef struct {
    int    conns_opened;
    int    requests_made;
    uint64_t bytes_received;
    int    header_ok;          /* every header read returned 206 w/ full body    */
    int    bulk_ok;            /* bulk covered [65536, total) with no gaps        */
    int    crc_404;            /* sidecar CRC returned 404 (or check disabled)    */
    int    total_bytes_expected; /* == total_size if all delivered                 */
    int    failures;           /* response mismatches (non-206, short body, ...)  */
    uint64_t total_delivered;  /* bytes received across the whole run             */
} ps5_sim_stats_t;

/*
 * Replay the PS5 pattern against the target stream server.
 * Returns 0 when every phase behaved as the model expects (all 206s with
 * full bodies, CRC sidecar 404, whole file delivered), non-zero otherwise.
 * The replay log (tab-separated, identical shape to the captured
 * .for_reference logs) is written to `log_fp` when non-NULL.
 */
int ps5_sim_replay(const ps5_sim_target_t *t, ps5_sim_stats_t *out, FILE *log_fp);

#endif /* PS5_SIM_H */
