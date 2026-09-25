# SMB authentication and throughput diagnostics

## Read-only host probe

Requirements: a C compiler, CMake, Python 3 and `patch`. Set `CMAKE` to the CMake
executable if it is not on PATH. Run from the repository root:

```sh
# No credentials: test Guest, with anonymous fallback if rejected.
tools/run_smb_probe.sh smb://192.168.64.2/shared

# Credentials: test direct access, share enumeration and directory listing.
SMB_USER=user SMB_PASSWORD='your-password' tools/run_smb_probe.sh smb://192.168.64.2/shared

# Benchmark 1 GiB using the same 2 MiB caller buffer as an installation.
# SMB_VERIFY compares unaligned/random/tail/EOF reads with synchronous libsmb2.
SMB_USER=user SMB_PASSWORD='your-password' SMB_VERIFY=1 \
  tools/run_smb_probe.sh smb://192.168.64.2/shared/example.pkg 1024 2048
```

`SMB_DOMAIN` defaults to WORKGROUP. Passwords are not logged or written to app
settings. The probe reads existing files; it does not write to the share or install them.
Its output includes dialect, signing/encryption, credits, server maximum read,
wall-clock throughput in decimal MB/s and process CPU time. Build logs are under
`build/smb-probe/`. The build uses the pinned submodule plus tracked patches.

For an upstream crypto baseline, use `SMB_PROBE_BASELINE=1` and a separate
`SMB_PROBE_BUILD` directory. Add `SMB_PROBE_PORTABLE=1` to select the portable AES
backend used by the previous PS5 build. On macOS, `SMB_PROBE_ARCH=x86_64` with a
separate build directory runs the AES-NI path (Rosetta is required on ARM Macs).
Never compare outputs from a single build directory after switching backends.

## Windows guest access

Granting Everyone read permission does not enable guest authentication.
An enabled Guest account, network logon policy, password-protected sharing,
share permissions and filesystem permissions all affect access. Required SMB
signing/encryption is incompatible with guest sessions. See Microsoft's
[SMB signing documentation](https://learn.microsoft.com/en-us/windows-server/storage/file-server/smb-signing)
and [guest authentication explanation](https://techcommunity.microsoft.com/blog/filecab/smb-signing-and-guest-authentication/3846679).

Useful read-only checks in PowerShell on the Windows server:

```powershell
Get-SmbServerConfiguration | Select-Object RequireSecuritySignature, EncryptData
Get-LocalUser | Where-Object { $_.SID.Value -like '*-501' } | Select-Object Name, Enabled
Get-SmbShare -Name shared | Select-Object Name, Path, EncryptData
Get-SmbShareAccess -Name shared
```

For a dedicated guest test VM, configure guest network logon and turn off
password-protected sharing, ensure the Guest account is enabled and permitted
by both network logon policies, and use server policies that permit unsigned,
unencrypted guest access. A client's `AllowInsecureGuestAuth` setting does not
configure the Windows machine's SMB server. Credentialed signed connections
can be tested without these guest configuration changes.

The application now authenticates `Guest` with an explicit empty password.
libsmb2 treats an unset password as anonymous NTLM, a different authentication
mode. Blank app credentials permit one anonymous retry after a logon rejection;
explicit credentials never fall back to guest/anonymous. A disabled account
gets a specific error. Transport failures and nonexistent shares are not retried.

Windows may also deny share enumeration over IPC$ while permitting direct
access to a known share. The setup form accepts a typed share name so enumeration
is optional. Use **Test connection** to verify that share and its folder.

## Windows 11 VM investigation, 2026-09-25

Target: `192.168.64.2`, share `shared`. Credentialed connections negotiated
SMB 3.1.1, required signing, no encryption, an 8 MiB maximum read and 1,023
credits after connection. Both libsmb2 and the macOS SMB client accepted the
supplied test account. Both rejected guest access.

The original anonymous request returned `STATUS_ACCESS_DENIED` (`0xC0000022`).
An explicit empty-password Guest request returned `STATUS_ACCOUNT_DISABLED`
(`0xC0000072`). The VM owner confirmed only Everyone permissions had been
configured. Successful Windows guest access therefore remains unverified until
the VM is configured for guest logon.

Measured through `smb_file_session_read`, using 2 MiB caller buffers and the same
existing file. Each row below is one 256 MiB signed transfer on the development
Mac; the portable baseline uses the previous PS5 AES implementation:

| Signing implementation | MB/s | Process CPU seconds |
| --- | ---: | ---: |
| Upstream portable AES | 73.36 | 2.630 |
| Upstream Apple backend | 95.61 | 1.704 |
| Patched Apple backend | 176.39 | 0.458 |
| Patched x86 AES-NI under Rosetta | 191.64 | 0.533 |

A subsequent 1 GiB patched Apple run with read verification sustained 173.57 MB/s.
The x86 run also passed read verification against the Windows server.
These are host-to-VM measurements over a virtual network, not PS5 speed claims
or physical gigabit Ethernet measurements. Disk cache and VM scheduling affect
results. The signing overhead is reproducible; whether it explains a particular
user's slowdown still requires their negotiated settings and on-console timing.

The previous library expanded an AES key (portable path), or created an Apple
cryptor, for every 16-byte block. The patch reuses setup for each message and
uses hardware AES on supported CPUs, while preserving signature verification.

For console validation, enable **PKG install debug**, then collect `/api/log`
and the SMB debug report. The session log includes `dialect`, `sign`, `seal`
and `max_read_size`. Compare the same package and server before/after, including
two parallel installation reads, cancellation and a subsequent install. Also
check an unsigned Linux/Samba share and a deliberately configured guest server.
