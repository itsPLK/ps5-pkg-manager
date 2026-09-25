# libsmb2 patches

`fast-cmac.patch` adds a fast path to the existing SMB3 AES-CMAC implementation.
`cmac_accel.h` reuses an AES key schedule for the entire message: AES-NI with a
runtime CPU check on x86, or one CommonCrypto context per message on Apple.
Other CPUs, or a failed backend initialization/update, use upstream's portable
implementation. The signing protocol, negotiated dialect, signature checks and
server signing requirements are unchanged.

`tools/prepare_libsmb2.py` copies the checked-in submodule to a build directory
and applies these files there. The submodule stays untouched. The PS5 Makefile
links `build/libsmb2/lib/libsmb2.a`, and searches the matching submodule headers
before SDK headers. Updating an SDK image is not required for these patches.

Validate both implementations against RFC 4493 and upstream:

```sh
tools/run_smb_probe.sh --test-cmac
# On an Apple Silicon Mac with Rosetta, exercise the PS5's AES-NI path:
SMB_PROBE_ARCH=x86_64 SMB_PROBE_BUILD=/tmp/smb-x86 tools/run_smb_probe.sh --test-cmac
# Exercise the portable fallback:
SMB_PROBE_PORTABLE=1 SMB_PROBE_BUILD=/tmp/smb-portable tools/run_smb_probe.sh --test-cmac
```

Use separate build directories for different architectures or backends. The
tests include empty/partial/complete blocks, unaligned input, large buffers and
four concurrent distinct keys. These crypto tests use real libsmb2, so they
are separate from `make test`, which uses the SMB transport mock.
