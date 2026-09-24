#!/bin/bash
# PKG Manager - Automated Build & Deploy Script

if [ -z "$1" ]; then
    echo "Usage: ./deploy.sh [PS5_IP]"
    exit 1
fi

PS5_IP="$1"
MENU_PORT="8844"
LOADER_PORT="9021"
ELF="pkgmgr.elf"
IMAGE_NAME="ps5-payload-sdk-pkgmgr"

echo "--- Deploying PKG Manager to $PS5_IP ---"

# 1. Build the React Frontend
echo "[1/3] Building React Frontend..."
make frontend-build > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "      !!! Frontend build FAILED!"
    exit 1
fi
echo "      Frontend build successful."

# 2. Build native ELF via Docker (keep output visible; it contains the
# helper/loader compile lines needed to confirm what actually rebuilt)
echo "[2/3] Building native ELF via Docker..."
# Force embedded-helper regeneration regardless of mtimes: blob.S incbins
# build/install-helper.elf, so a stale helper silently ships old behavior.
rm -f build/install-helper.elf
docker run --rm -v "$(pwd)":/src -w /src $IMAGE_NAME make clean all 2>&1 | tee build-docker.log

if [ "${PIPESTATUS[0]}" -ne 0 ]; then
    echo "      !!! ELF build FAILED! See build-docker.log."
    exit 1
fi
echo "      ELF build successful."

# 2b. Verify the embedded helper was actually regenerated.
if [ ! -f "build/install-helper.elf" ]; then
    echo "      !!! build/install-helper.elf missing after build! See build-docker.log."
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

# 3. Send to PS5
if [ -f "$ELF" ]; then
    echo "[3/3] Sending $ELF to $PS5_IP:$LOADER_PORT via socat..."
    socat -u - TCP:$PS5_IP:$LOADER_PORT < "$ELF"
    if [ $? -eq 0 ]; then
        echo "--- Deployment Complete! ---"
    else
        echo "      !!! Failed to send ELF. Is the loader running on PS5?"
        exit 1
    fi
else
    echo "      !!! $ELF not found!"
    exit 1
fi
