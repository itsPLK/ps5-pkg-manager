# Development

This document describes how to build, test, and deploy **PKG Manager**.

## Getting Started

Clone the repository with submodules:
```bash
git clone --recurse-submodules https://github.com/itsPLK/ps5-pkg-manager.git
# Or if already cloned:
git submodule update --init --recursive
```

## How to Build


### 1. Build the Frontend
You must build the React UI first. This compiles the JSX into the single-file bundle that gets converted into C header assets:
```bash
make frontend-build
```

### 2. Build the SDK Docker Image
If you haven't already, build the PS5 payload SDK Docker container:
```bash
docker build -t ps5-payload-sdk-pkgmgr -f Dockerfile.sdk .
```

### 3. Build the ELF
Compile the native ELF using the Docker container. It is recommended to run `make clean` before rebuilding if headers or frontend changed:
```bash
docker run --rm -v $(pwd):/src -w /src ps5-payload-sdk-pkgmgr make clean all
```

The resulting `pkgmgr.elf` will be created in the root directory.

The build also creates `build/install-helper.elf` and embeds it in `pkgmgr.elf`.
Only `pkgmgr.elf` is deployed. The install helper launches directly; no elfldr
service, loader port, or external helper file is required at runtime.

### 4. Build a Versioned Development Binary
To build a versioned development binary (`pkg-manager_v<VERSION>-dev-<SHORT_HASH>.elf`):
```bash
./build_release.sh
```

## Running Unit Tests

You can run the full host test suite locally without Docker:
```bash
make test
```

`make test-install-service` exercises the actual helper protocol in freshly
executed host processes with stubbed PS5 APIs. It covers consecutive installs,
single-submission enforcement, native failures, malformed IPC, helper death,
cancellation, parent disconnection, and graceful/forced child cleanup. This
does not emulate PS5 `rfork`/ptrace or prove the firmware issue is resolved.

For console validation, install at least two packages without restarting the
manager, then exercise a base/update batch, consecutive Direct Installs, and a
cancel followed by another install. The manager PID should stay constant; each
attempt should report a distinct helper PID. Check both older firmware and an
affected newer firmware.

### Collecting install diagnostics from users

Enable **PKG install debug** before reproducing the issue. Collect the generated
`stream_debug_*.txt` report from `/data/pkgmgr/` (or `PKG_DEBUG_DIR`) and the full
`/api/log` response. Reports retain their existing HTTP/WS events and now also
contain timestamped `INSTALL_EVENT` records. A single report stays open across
stream retries and through helper cleanup; names include PID and sequence to
avoid overwriting same-second attempts. Retention remains 20 stream reports.

Helper diagnostics record the raw firmware query, daemon/helper PIDs, embedded
helper size/checksum/build, launch stages, IPC protocol sizes, native return
codes and durations, request URLs, content IDs, status/progress snapshots,
native error descriptions, timeout/cancel events, and process exit/signal
status. Native status snapshots are written on changes and every five seconds.
The report closes after the final outcome and helper cleanup.

`/api/log` retains **65,536 lines** (previously 2,048), up to **32 MiB** in memory,
and returns the full retained history. The ordinary `install.log` continues to
store warnings/errors only; the debug report contains successful transitions
needed to reconstruct an install attempt.

This compiles and runs tests for:
- Package parser (`test_pkg_parser`)
- Drive and package scanner with manifest caching (`test_pkg_scanner`)
- Package cache and settings (`test_pkg_cache`)
- Installer state machine and space checks (`test_installer`)
- Orphaned update and DLC detection (`test_leftovers`)
- Edge cases and error handling (`test_edge_cases`)
- Multi-part packages and virtual stream engine (`test_multipart`)
- PS5 installer stream simulation (`test_stream_sim`) — pairs the PS5
  request-pattern simulator (`tests/ps5_sim.c`) against the real stream
  server (`src/stream_server.c`) on the host; no PS5 required (see below)
- Direct Install WebSocket transport and live-stream tests (`test_ws_upload`,
  `test_ws_stream`, `test_ws_stream_far`, `test_direct_install_e2e`)
- In-memory package parsing (`test_parse_mem`)
- Legacy CSS syntax transformer (`test_fix_legacy_css.py`)

### Large SMB share regressions

`make test TESTS=test_smb_scan` generates 3,000 small synthetic PKGs in separate
games, updates and DLC folders, using the real scanner/parser over the local SMB
transport mock. It checks:

- overlapping full scans perform one traversal (the original implementation
  reproduced two traversals: 16 directory opens instead of 8);
- background admission, progress and catalog access during directory I/O;
- directory enumeration closes before metadata reads;
- cursor paging reaches every file in a 1,000-entry folder without opening PKGs;
- a nested directory failure preserves the quick-scan catalog and reports a full
  scan error;
- browse-only settings persist, perform no scan I/O, remove previously indexed
  entries, and still allow parsing an individually selected file.

`cd frontend && npm test` covers scan polling through a transient connection
failure, reconnecting without another scan request, manual browse/inspect requests,
and rendering 60 cards from 3,000 titles, including the last page.

These tests validate application behavior with a mocked SMB transport. A live
PS5/NAS run is still needed to confirm console memory limits, server timeouts,
controller interaction and installation from the reporter's share.

### PS5 Installer Stream Simulator

`test_stream_sim` reproduces the exact HTTP request pattern the PS5 background
package installer sends to the stream server (`:18841`), so install methods
(websocket client, direct stream creation) can be developed and verified on the
host without a console. It is modeled from the captures in
`.for_reference/stream_debug/`: a burst of header re-reads, a `*.crc` sidecar
probe that must 404, then two parallel bulk connections serving contiguous
16 MiB byte-ranges.

- It is built and run automatically by `make test` (listed in `TESTS`, with
  `tests/ps5_sim.c` in `TEST_SRCS`).
- `tools/ps5_installer_sim.c` is the standalone CLI half — it can point the
  same replay at any live server (`--no-server --port 18841`), or be fully
  self-contained: build a fixture PKG, start a local stream server, replay the
  PS5 pattern, and print a reference-format replay log.
- `tools/run_stream_sim.sh` builds and runs the CLI:

  ```bash
  tools/run_stream_sim.sh --demo          # build fixture, replay, print log
  tools/run_stream_sim.sh --no-server     # replay against an existing server
  ```

### Direct Install over WebSocket

The Direct Install page lets a LAN browser, such as a PC, push a local `.pkg` to the
daemon, which streams it straight into the installer from RAM — nothing
is stored on disk. Install can start as soon as the header is parsed,
while the rest still uploads:

- Transport: `src/ws_upload.c` (`include/ws_upload.h`) — RFC6455 listener
  on `:18842`, in-order chunks with resume. Narrow REST hook in
  `src/http_server.c` (`POST /api/upload/init|finish|cancel`,
  `GET /api/upload/status`); chunk bytes never go through MHD.
- Live session: `src/ws_stream.c` (`include/ws_stream.h`) — 1 MB pinned
  header cache + configurable RAM ring (64 MB default, `WS_LIVE_RING_MB`),
  blocking readers, bounded writer admission, and abort/timeout handling.
  Served through the existing HTTP range path via the `live:<id>`
  virtual-stream scheme (`src/multipart.c`, `src/stream_server.c`).
- Metadata: additive `pkg_parser_parse_mem()`; install entry
  `installer_start_live()`; `/api/install` routes `live:` URIs.
- Frontend: `DirectInstallView.jsx` + `api/directInstall.js` +
  `hooks/useDirectUpload.js` (Header "Direct Install" button and app-wide
  file drop). Install is
  enabled at `header_ready`, with sent/installed dual progress.
- Upload scheduling: the sender uploads the header first, then follows installer
  seeks with a bounded window of up to eight 1 MiB segments and up to two
  uploads in flight.
  Busy replies retry the same segment; requests for in-flight segments are
  coalesced. The WebSocket listener starts on demand and closes when idle.
- With install debug mode enabled, the install screen shows WebSocket receive
  and install speed graphs. Stream logs include build identity, receive and
  accepted throughput, cache duplicate/reload/eviction counters, and periodic
  browser file-read and send-to-ACK timing summaries. The selected debug
  directory keeps at most 20 stream logs and 20 SMB logs.
- Host tests (all in `make test`): `test_ws_stream` (ring unit),
  `test_parse_mem` (parse vs parse_mem differential),
  `test_ws_upload` (codec + socket + fragmentation), `test_direct_install_e2e`
  (concurrent push + PS5 pull, abort fail-fast, small-ring wrap,
  `installer_start_live` commit).
- Simulators: `tools/ws_push_sim.c` via `tools/run_direct_install_sim.sh`:

  ```bash
  tools/run_direct_install_sim.sh --demo                # concurrent push+pull
  tools/run_direct_install_sim.sh --demo --resume-test  # drop + resume
  tools/run_direct_install_sim.sh --pkg game.pkg --port 18842  # live server
  ```

- Frontend mock: `node frontend/mock-server.js` serves the same REST shape
  plus a memory-backed mock WS listener on `:18842`.
- The sender scheduler also has focused Node.js tests, separate from `make test`:
  ```bash
  cd frontend && npm run test:upload
  ```

## Automated Deploy

For a fast build and deploy cycle over the local network, use the `deploy.sh` script:
```bash
./deploy.sh [PS5_IP]
```
(Requires PS5 IP as the first argument; sends `pkgmgr.elf` via `socat` to port 9021).
