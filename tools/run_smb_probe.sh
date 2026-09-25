#!/usr/bin/env bash
# Build the real SMB transport for read-only host diagnostics (no PS5 required).
set -euo pipefail
cd "$(dirname "$0")/.."
repo="$PWD"
cmake_bin="${CMAKE:-cmake}"
probe_cc="${CC:-cc}"
probe_build="${SMB_PROBE_BUILD:-$repo/build/smb-probe}"
mkdir -p "$probe_build"
probe_build="$(cd "$probe_build" && pwd)"
source_dir="$probe_build/source"
if [[ "${SMB_PROBE_BASELINE:-0}" == 1 ]]; then
    source_dir="$repo/deps/libsmb2"
else
    python3 tools/prepare_libsmb2.py "$source_dir"
fi
args=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
if [[ "${SMB_PROBE_PORTABLE:-0}" == 1 ]]; then
    args+=(-DHAVE_COMMONCRYPTO_COMMONCRYPTOR_H=0 -DCMAKE_C_FLAGS=-DPKGMGR_CMAC_PORTABLE)
else
    args+=(-DCMAKE_C_FLAGS=)
fi
if [[ -n "${SMB_PROBE_ARCH:-}" ]]; then
    args+=("-DCMAKE_OSX_ARCHITECTURES=$SMB_PROBE_ARCH")
fi
"$cmake_bin" -S "$source_dir" -B "$probe_build/lib-build" "${args[@]}" > "$probe_build/configure.log" 2>&1
"$cmake_bin" --build "$probe_build/lib-build" -j 4 > "$probe_build/build.log" 2>&1
flags=(-O2 -ffunction-sections -fdata-sections -Iinclude -Ideps/libsmb2/include -Ideps/libsmb2/include/smb2)
if [[ -n "${SMB_PROBE_ARCH:-}" ]]; then flags+=(-arch "$SMB_PROBE_ARCH"); fi
if [[ "$(uname -s)" == Darwin ]]; then link_flags=(-Wl,-dead_strip); else link_flags=(-Wl,--gc-sections); fi
lib="$probe_build/lib-build/lib/libsmb2.a"
if [[ "${1:-}" == --test-cmac ]]; then
    "$probe_cc" "${flags[@]}" -DHAVE_CONFIG_H -I"$probe_build/lib-build" -Ideps/libsmb2/lib \
        -Dsmb3_aes_cmac_128=reference_cmac -Dsmb2_calc_signature=reference_calc_signature \
        -Dsmb2_pdu_add_signature=reference_add_signature -Dsmb2_pdu_check_signature=reference_check_signature \
        -c deps/libsmb2/lib/smb2-signing.c -o "$probe_build/reference-signing.o"
    "$probe_cc" "${flags[@]}" "${link_flags[@]}" tests/test_smb_cmac.c "$probe_build/reference-signing.o" "$lib" -lpthread -o "$probe_build/test_smb_cmac"
    exec "$probe_build/test_smb_cmac"
fi
"$probe_cc" "${flags[@]}" "${link_flags[@]}" tools/smb_probe.c src/smb_debug_log.c src/debug_log_retention.c "$lib" -lpthread -o "$probe_build/smb_probe"
exec "$probe_build/smb_probe" "$@"
