#!/bin/sh
#
# run_stream_sim.sh — build and run the PS5 installer stream simulator.
#
# Compiles tools/ps5_installer_sim.c (the PS5 request-pattern client half,
# paired against the real stream_server.c) and runs it. Without arguments it
# builds a small fixture PKG, starts a local stream server on :18841, replays
# the PS5 install pattern, prints a reference-format replay log, and stops.
#
# Pass any options through to the simulator, e.g.:
#   tools/run_stream_sim.sh --demo
#   tools/run_stream_sim.sh --no-server --port 18841
#   tools/run_stream_sim.sh --parallel 2 --header-repeats 5 --size 104857600
#
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/ps5_installer_sim"

CC="${CC:-cc}"
CFLAGS="-O0 -Iinclude -Itests -Ideps/libsmb2/include -Ideps/libsmb2/include/smb2 \
-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_WAL"

echo "=== Building PS5 installer stream simulator -> ${OUT} ==="
"${CC}" ${CFLAGS} -o "${OUT}" \
    tools/ps5_installer_sim.c \
    src/multipart.c src/pkg_parser.c src/pkg_scanner.c src/pkg_cache.c \
    src/miniz.c src/smb_client.c src/installer.c src/stream_server.c \
    src/stream_debug_log.c src/notification.c src/app_info.c \
    src/icon_blurhash.c src/leftovers.c src/app_diag.c src/app_installer.c \
    src/sqlite3.c tests/mock_smb.c tests/ps5_sim.c \
    src/ws_upload.c src/ws_stream.c tests/ws_test_client.c -lpthread -lm -ldl

echo "=== Running ${OUT} $* ==="
"${OUT}" "$@"
