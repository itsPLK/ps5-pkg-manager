#!/bin/sh
#
# run_direct_install_sim.sh — build and run the WS push simulator.
#
# Self-contained by default: builds a fixture PKG, starts the real
# ws_direct listener + stream server on the host, pushes the fixture over a
# real WebSocket (as the DirectInstallView page would), then replays the PS5
# pull pattern against the spooled file. No PS5 required.
#
# Pass options through to the simulator, e.g.:
#   tools/run_direct_install_sim.sh --demo
#   tools/run_direct_install_sim.sh --demo --resume-test
#   tools/run_direct_install_sim.sh --pkg game.pkg --host 127.0.0.1 --port 8846
#
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/ws_push_sim"

CC="${CC:-cc}"
CFLAGS="-O0 -Iinclude -Itests -Ideps/libsmb2/include -Ideps/libsmb2/include/smb2 \
-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_WAL"

echo "=== Building WS push simulator -> ${OUT} ==="
"${CC}" ${CFLAGS} -o "${OUT}" \
    tools/ws_push_sim.c \
    src/multipart.c src/pkg_parser.c src/pkg_scanner.c src/pkg_cache.c \
    src/miniz.c src/smb_client.c src/installer.c src/stream_server.c \
    src/stream_debug_log.c src/notification.c src/app_info.c \
    src/icon_blurhash.c src/leftovers.c src/app_diag.c src/app_installer.c \
    src/sqlite3.c tests/mock_smb.c tests/ps5_sim.c \
    src/ws_upload.c src/ws_stream.c tests/ws_test_client.c -lpthread -lm -ldl

if [ "$#" -eq 0 ]; then
    set -- --demo
fi

echo "=== Running ${OUT} $* ==="
"${OUT}" "$@"
