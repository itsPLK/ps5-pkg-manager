#!/bin/bash
# PKG Manager - Versioned Build Script
# WORKDIR is /src (see Dockerfile.sdk WORKDIR); mount with -v "$(pwd)":/src -w /src.
set -euo pipefail

# 1. Extract version from include/version.h
VERSION=$(grep '#define PKGMGR_VERSION' include/version.h | awk '{print $3}' | tr -d '"' | tr -d '\r')

if [ -z "$VERSION" ]; then
    echo "Error: Could not find PKGMGR_VERSION in include/version.h"
    exit 1
fi

SHORT_HASH=$(git rev-parse --short HEAD 2>/dev/null || true)
if [ -z "$SHORT_HASH" ]; then
    echo "Error: Could not determine the current Git commit"
    exit 1
fi

OUTPUT_ELF="pkg-manager_v${VERSION}-dev-${SHORT_HASH}.elf"
IMAGE_NAME="ps5-payload-sdk-pkgmgr"

echo "--- Building PKG Manager v$VERSION ---"

# 2. Build React Frontend (on Host)
echo "[1/3] Building React Frontend..."
make frontend-build
echo "      Frontend build successful."

# 2b. Build/verify the docker image
if [[ "$(docker images -q $IMAGE_NAME 2> /dev/null)" == "" ]]; then
    echo "      Docker image $IMAGE_NAME not found. Building... (this may take a few minutes)"
    docker build -t $IMAGE_NAME -f Dockerfile.sdk .
    if [ $? -ne 0 ]; then
        echo "      !!! Docker image build FAILED!"
        exit 1
    fi
    echo "      Docker image built successfully."
fi

# 3. Build native ELF via Docker
echo "[2/3] Building native ELF via Docker..."
# Force embedded-helper regeneration regardless of mtimes: blob.S incbins
# build/install-helper.elf, so a stale helper silently ships old behavior.
rm -f build/install-helper.elf
docker run --rm -e BUILD_COMMIT="$SHORT_HASH" -v "$(pwd)":/src -w /src $IMAGE_NAME make clean all

echo "      ELF build successful."

# 3b. Verify the embedded helper was actually regenerated.
if [ ! -f "build/install-helper.elf" ]; then
    echo "      !!! build/install-helper.elf missing after build!"
    exit 1
fi
ls -l build/install-helper.elf
python3 - <<'EOF'
d = open('build/install-helper.elf','rb').read()
h = 2166136261
for b in d:
    h = ((h ^ b) * 16777619) & 0xFFFFFFFF
print('      helper bytes=%d fnv1a=0x%08X (compare with runtime embedded_fnv1a)' % (len(d), h))
EOF

# 4. Rename output
if [ -f "pkgmgr.elf" ]; then
    mv pkgmgr.elf "$OUTPUT_ELF"
    echo "[3/3] Created versioned binary: $OUTPUT_ELF"
    echo "--- Build Complete! ---"
else
    echo "      !!! pkgmgr.elf not found after build!"
    exit 1
fi
