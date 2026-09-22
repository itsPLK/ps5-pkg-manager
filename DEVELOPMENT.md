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

### 4. Build a Versioned Release
To build a versioned release binary (`pkgmgr_v<VERSION>.elf`):
```bash
./build_release.sh
```

## Running Unit Tests

You can run the full host test suite locally without Docker:
```bash
make test
```

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
  header cache + 64 MB ring (`WS_LIVE_RING_MB`), blocking readers,
  backpressure on writers, abort/timeout on every wait. Served through
  the existing pipeline via the `live:<id>` virtual-stream scheme
  (`src/multipart.c`), so `src/stream_server.c` is untouched.
- Metadata: additive `pkg_parser_parse_mem()`; install entry
  `installer_start_live()`; `/api/install` routes `live:` URIs.
- Frontend: `DirectInstallView.jsx` + `api/directInstall.js` +
  `hooks/useDirectUpload.js` (Header "Direct Install" button and app-wide
  file drop). Install is
  enabled at `header_ready`, with sent/installed dual progress.
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

## Automated Deploy

For a fast build and deploy cycle over the local network, use the `deploy.sh` script:
```bash
./deploy.sh [PS5_IP]
```
(Requires PS5 IP as the first argument; sends `pkgmgr.elf` via `socat` to port 9021).
