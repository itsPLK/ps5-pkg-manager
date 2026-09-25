# PKG Manager Makefile

PYTHON := python3
CC     := /opt/ps5-payload-sdk/bin/prospero-clang
AR     := /opt/ps5-payload-sdk/bin/prospero-ar
RANLIB := /opt/ps5-payload-sdk/bin/prospero-ranlib
STRIP  := /opt/ps5-payload-sdk/bin/prospero-strip

SDK      := /opt/ps5-payload-sdk
TARGET   := $(SDK)/target
# Always build the checked-in revision and tracked patches. The SDK may hold
# an older library with different private structs and unaccelerated signing.
LIBSMB2  ?= build/libsmb2/lib/libsmb2.a
SMB2_CMAKE ?= $(SDK)/bin/prospero-cmake
SMB2_INPUTS := $(wildcard deps/libsmb2/lib/*.[ch] deps/libsmb2/include/*.h deps/libsmb2/include/smb2/*.h deps/libsmb2/libdcerpc/*.[ch] deps/libsmb2/cmake/* deps/libsmb2/cmake/Modules/*) deps/libsmb2/CMakeLists.txt deps/libsmb2/lib/CMakeLists.txt
INCLUDES := -Iinclude -Ideps/libsmb2/include -Ideps/libsmb2/include/smb2 -I$(TARGET)/include
LIBS     := $(TARGET)/lib/libmicrohttpd.a \
            $(LIBSMB2) \
            -L$(TARGET)/lib -lpthread \
            -lSceNetCtl -lSceUserService -lSceSystemService \
            -lSceAppInstUtil -lSceNet

SRCS := src/main.c src/pkg_parser.c src/pkg_scanner.c src/pkg_cache.c src/smb_client.c src/smb_debug_log.c src/debug_log_retention.c src/installer.c \
        src/http_server.c src/stream_server.c src/stream_debug_log.c src/notification.c \
        src/multipart.c src/miniz.c src/app_info.c src/sqlite3.c src/icon_blurhash.c src/leftovers.c src/app_diag.c \
        src/app_installer.c
# Direct-install WebSocket upload modules.
SRCS_WS := src/ws_upload.c src/ws_stream.c
SRCS_INSTALL_SERVICE := src/install_service.c src/install_ipc.c \
                        src/install_process.c src/install_helper_blob.S
INSTALL_HELPER := build/install-helper.elf
OBJS := $(SRCS:.c=.o)
ELF  := pkgmgr.elf

GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GIT_DIRTY  := $(shell git diff --quiet 2>/dev/null || echo "-dirty")
BUILD_COMMIT ?= $(GIT_COMMIT)$(GIT_DIRTY)
BUILD_DATE   ?= $(shell date -u +"%Y-%m-%d_%H:%M:%S_UTC")

FRONTEND_DIST := frontend/dist/index.html
ASSET_HEADER  := include/assets_index_html.h
MANIFEST_DIST := frontend/dist/cache.appcache
MANIFEST_HEADER := include/assets_cache_appcache.h
FAVICON_SVG_DIST   := frontend/dist/favicon.svg
FAVICON_SVG_HEADER := include/assets_favicon_svg.h
ICON_PNG_DIST      := frontend/dist/icon.png
ICON_PNG_HEADER    := include/assets_icon_png.h
PARAM_JSON_DIST    := assets/param.json
PARAM_JSON_HEADER  := include/assets_param_json.h
ICON0_PNG_DIST     := assets/icon0.png
ICON0_PNG_HEADER   := include/assets_icon0_png.h
ASSET_HEADERS      := $(ASSET_HEADER) $(MANIFEST_HEADER) $(FAVICON_SVG_HEADER) $(ICON_PNG_HEADER) $(PARAM_JSON_HEADER) $(ICON0_PNG_HEADER)

CFLAGS := -Os -Wall -Wno-visibility -DPS5_BUILD -D_BSD_SOURCE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_WAL -DPKGMGR_BUILD_COMMIT=\"$(BUILD_COMMIT)\" -DPKGMGR_BUILD_DATE=\"$(BUILD_DATE)\" -ffunction-sections -fdata-sections $(INCLUDES)
LDFLAGS := -Wl,--gc-sections

# Host test build (uses tests/mock_smb.c instead of real libsmb2; MHD not needed)
TEST_CFLAGS := -g -O0 -Wall -Wextra -Iinclude -Ideps/libsmb2/include -Ideps/libsmb2/include/smb2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=2 -DSQLITE_OMIT_WAL -DPKGMGR_BUILD_COMMIT=\"$(BUILD_COMMIT)\" -DPKGMGR_BUILD_DATE=\"$(BUILD_DATE)\"
TEST_SRCS := src/multipart.c src/pkg_parser.c src/pkg_scanner.c src/pkg_cache.c src/miniz.c src/smb_client.c src/smb_debug_log.c src/debug_log_retention.c src/installer.c src/stream_server.c src/stream_debug_log.c src/notification.c src/app_info.c src/icon_blurhash.c src/leftovers.c src/app_diag.c src/app_installer.c src/sqlite3.c tests/mock_smb.c tests/ps5_sim.c src/ws_upload.c src/ws_stream.c tests/ws_test_client.c
TESTS := test_smb_auth test_smb_scan test_pkg_parser test_pkg_scanner test_pkg_cache test_installer test_leftovers test_edge_cases test_multipart test_stream_sim test_ws_upload test_direct_install_e2e test_ws_stream test_ws_stream_far test_parse_mem

all: $(ELF)

.PHONY: frontend-build
frontend-build:
	@echo "Building frontend..."
	@VERSION=$$(grep '#define PKGMGR_VERSION' include/version.h | awk '{print $$3}' | tr -d '"'); \
	COMMIT=$$(git rev-parse --short HEAD 2>/dev/null || echo "unknown"); \
	DATE=$$(date -u +"%Y-%m-%d %H:%M:%S UTC"); \
	TITLE="PKG Manager v$$VERSION ($$COMMIT, $$DATE) by PLK"; \
	echo "Updating title in index.html to: $$TITLE"; \
	(cd frontend && npm ci && VITE_APP_VERSION="$$VERSION" VITE_APP_COMMIT="$$COMMIT" VITE_APP_BUILD_DATE="$$DATE" npm run build); \
	TMP=$$(mktemp "$${TMPDIR:-/tmp}/pkgmgr.XXXXXX"); \
	sed -e "s|\[\[TITLE_PLACEHOLDER\]\]|$$TITLE|g" -e "s|<title>.*</title>|<title>$$TITLE</title>|g" frontend/dist/index.html > $$TMP; \
	mv $$TMP frontend/dist/index.html; \
	chmod 644 frontend/dist/index.html; \
	echo "Updating build date in cache.appcache to: $$DATE"; \
	TMP=$$(mktemp "$${TMPDIR:-/tmp}/pkgmgr.XXXXXX"); \
	sed "s|\[\[BUILD_DATE\]\]|$$DATE|g" frontend/dist/cache.appcache > $$TMP; \
	mv $$TMP frontend/dist/cache.appcache; \
	chmod 644 frontend/dist/cache.appcache
	@echo "Rewriting modern rgb() slash syntax for Safari 12..."
	$(PYTHON) tools/fix_legacy_css.py $(FRONTEND_DIST)
	@echo "Generating asset headers..."
	$(PYTHON) tools/gen_assets.py $(FRONTEND_DIST) $(ASSET_HEADER) index_html
	$(PYTHON) tools/gen_assets.py $(MANIFEST_DIST) $(MANIFEST_HEADER) cache_appcache
	$(PYTHON) tools/gen_assets.py $(FAVICON_SVG_DIST) $(FAVICON_SVG_HEADER) favicon_svg
	$(PYTHON) tools/gen_assets.py $(ICON_PNG_DIST) $(ICON_PNG_HEADER) icon_png
	$(PYTHON) tools/gen_assets.py $(PARAM_JSON_DIST) $(PARAM_JSON_HEADER) param_json
	$(PYTHON) tools/gen_assets.py $(ICON0_PNG_DIST) $(ICON0_PNG_HEADER) icon0_png

$(ASSET_HEADER): $(FRONTEND_DIST)
	$(PYTHON) tools/gen_assets.py $(FRONTEND_DIST) $(ASSET_HEADER) index_html

$(MANIFEST_HEADER): $(FRONTEND_DIST)
	$(PYTHON) tools/gen_assets.py $(MANIFEST_DIST) $(MANIFEST_HEADER) cache_appcache

$(FAVICON_SVG_HEADER): $(FAVICON_SVG_DIST)
	$(PYTHON) tools/gen_assets.py $(FAVICON_SVG_DIST) $(FAVICON_SVG_HEADER) favicon_svg

$(ICON_PNG_HEADER): $(ICON_PNG_DIST)
	$(PYTHON) tools/gen_assets.py $(ICON_PNG_DIST) $(ICON_PNG_HEADER) icon_png

$(PARAM_JSON_HEADER): $(PARAM_JSON_DIST)
	$(PYTHON) tools/gen_assets.py $(PARAM_JSON_DIST) $(PARAM_JSON_HEADER) param_json

$(ICON0_PNG_HEADER): $(ICON0_PNG_DIST)
	$(PYTHON) tools/gen_assets.py $(ICON0_PNG_DIST) $(ICON0_PNG_HEADER) icon0_png

$(FRONTEND_DIST):
	@echo "ERROR: frontend/dist/index.html not found! Run 'make frontend-build' first."
	@exit 1

build/libsmb2-src/.prepared: tools/prepare_libsmb2.py $(wildcard patches/libsmb2/*) $(SMB2_INPUTS)
	$(PYTHON) tools/prepare_libsmb2.py build/libsmb2-src

build/libsmb2/lib/libsmb2.a: build/libsmb2-src/.prepared
	$(SMB2_CMAKE) -S build/libsmb2-src -B build/libsmb2 -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF
	$(MAKE) -C build/libsmb2 -j$$(getconf _NPROCESSORS_ONLN)

$(INSTALL_HELPER): Makefile src/install_helper.c src/install_ipc.c include/install_ipc.h include/install_service.h include/install_appinst.h include/version.h
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/install_helper.c src/install_ipc.c -lpthread \
		-lSceNetCtl -lSceUserService -lSceSystemService -lSceAppInstUtil -lSceNet
	$(STRIP) $@

$(ELF): $(ASSET_HEADERS) $(LIBSMB2) $(SRCS) $(SRCS_WS) $(SRCS_INSTALL_SERVICE) $(INSTALL_HELPER) $(wildcard include/*.h)
	@echo "Building $(ELF)..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(ELF) $(SRCS) $(SRCS_WS) $(SRCS_INSTALL_SERVICE) $(LIBS)
	@echo "Stripping $(ELF)..."
	$(STRIP) $(ELF)

clean:
	rm -f $(ELF) $(INSTALL_HELPER) pkgmgr_v*.elf pkg-manager_v*.elf $(ASSET_HEADERS) src/*.o $(addprefix tests/,$(TESTS))
	rm -rf $(addprefix tests/,$(addsuffix .dSYM,$(TESTS)))

test-install-service:
	mkdir -p build
	cc $(TEST_CFLAGS) -DINSTALL_HELPER_TEST -o build/test_install_service tests/test_install_service.c src/install_service.c src/install_ipc.c src/install_helper.c -lpthread
	./build/test_install_service
	cc $(TEST_CFLAGS) -o build/test_install_process tests/test_install_process.c tests/install_helper_fixture.S src/install_process.c src/install_ipc.c -lpthread
	./build/test_install_process

test: $(PARAM_JSON_HEADER) $(ICON0_PNG_HEADER) test-install-service
	@for t in $(TESTS); do \
		echo "=== CC tests/$$t ==="; \
		cc $(TEST_CFLAGS) -o tests/$$t tests/$$t.c $(TEST_SRCS) -lpthread -lm -ldl || exit 1; \
	done
	@for t in $(TESTS); do \
		echo "=== RUN tests/$$t ==="; \
		./tests/$$t || exit 1; \
	done
	@echo "=== RUN tests/test_fix_legacy_css.py ==="
	$(PYTHON) tests/test_fix_legacy_css.py

dist-clean: clean
	rm -rf frontend/dist frontend/node_modules

.PHONY: all clean test test-install-service frontend-build dist-clean mock
mock:
	node frontend/mock-server.js
