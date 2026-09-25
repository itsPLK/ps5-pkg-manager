<p align="center">
  <img src="./assets/icon0.png" width="128" />
</p>
<h1 align="center">PKG Manager</h1>

<p align="center">A clean and intuitive package manager for PlayStation 5. Browse and install your packages directly from USB drives or over your local network (Samba/SMB), with support for multi-part packages.</p>

| | |
|:---:|:---:|
| **Select Storage Media**<br>![Select Storage Media](assets/screenshots/select-storage.webp) | **Package Browser**<br>![Package Browser](assets/screenshots/all-sources.webp) |
| **Package Details & Add-ons**<br>![Package Details](assets/screenshots/package-details.webp) | **Installation Progress**<br>![Installation Progress](assets/screenshots/install-screen.webp) |
| **Multi-Part Disc Swapping**<br>![Multi-Part Disc Swapping](assets/screenshots/insert-disc.webp) | **Samba (SMB) Settings**<br>![Samba Network Shares](assets/screenshots/settings-samba.webp) |

## Features

- **Clean Web Interface**: Displays your packages with package titles, icons, and version information.
- **USB & Network (Samba/SMB) Support**: Automatically detects packages on connected USB drives, or stream them over your local network from a PC or NAS via Samba shares.
- **Direct Install**: Install a local PKG file from another device on your network, such as a PC, directly to the console.
- **Multi-Part Packages & Disc Swapping**: Install large packages split across multiple optical discs or USB drives, with on-screen prompts when swapping discs. Ideal for physical backups!
- **No Duplicate Storage Needed**: Installs packages directly on the fly without requiring double the storage space for temporary copy files.
- **Installed Version Detection**: Checks your console to display installed application versions and prevent duplicate package installs.
- **Home Screen Shortcut**: Installs a dedicated shortcut tile to your PS5 home screen for quick and easy access.
- **Leftover Cleanup**: Detects and cleans up orphaned files and directories commonly left behind on console storage after a database rebuild.

## Installation

### Recommended: Payload Manager

Download and launch PKG Manager from the default repository in [Payload Manager](https://github.com/itsPLK/ps5-payload-manager).

### Manual ELF Loading

Alternatively, download the ELF from [Releases](https://github.com/itsPLK/ps5-pkg-manager/releases) and load it with elfldr.

## Usage

### Accessing the Interface
Once running, open the interface in either of the following ways:
- Launch the **PKG Manager** shortcut tile directly from the PS5 home screen.
- Open `http://[PS5_IP]:8844` in any web browser on a phone, tablet, or PC connected to the same local network.

### Package Locations
When using a USB drive or optical disc, packages are detected in:
- The **root** directory of the drive (nested folders in root are not scanned).
- The **/pkg/** directory, where nested subdirectories are also scanned (e.g. `/pkg/homebrew/`).

### Network Shares (Samba / SMB)
You can configure SMB network shares in **Settings → Samba** to browse and
install packages stored on your PC or NAS. Network shares are not refreshed
automatically; use the **Rescan** button to update the package catalog after
adding new files.

For Windows shares without a username or password, see the
[Windows 11 guest-sharing FAQ](#how-do-i-connect-to-a-windows-11-share-without-a-password).

### Direct Install
From another device on the same network, open the PKG Manager interface and choose **Direct Install**. Select or drop a local `.pkg` file to install it directly on the console.

### Multi-Part Packages
If you want to back up large packages onto optical discs (Blu-ray, DVD) or are limited by storage media size, you can split your package into multi-part files using the included tool:

```bash
python3 tools/pkg_split.py /path/to/package.pkg -s 23G
```

Multi-part packages can be burned across multiple discs or loaded directly from a USB drive. When installing from discs, the installer will automatically detect inserted media and prompt you with on-screen notifications whenever a disc swap is needed.

## FAQ

### How do I connect to a Windows 11 share without a password?

Guest sharing allows other devices on your local network to read shared
packages without entering credentials. Configure the following on the
**Windows PC hosting the files**:

1. **Enable file sharing & turn off password protection.** Set the PC's network
   profile to **Private**. Open **Settings → Network & internet → Advanced
   network settings → Advanced sharing settings**. Under **Private networks**,
   enable **Network discovery** and **File and printer sharing**. Under **All
   networks**, turn off **Password protected sharing**. Ensure File and Printer
   Sharing is allowed through Windows Firewall for the private network.

2. **Share the folder with read access.** Right-click your package folder, open
   **Properties → Sharing → Advanced Sharing**, enable **Share this folder**,
   and name it (e.g. `PS5PKG`). In **Permissions**, grant **Everyone → Read**.
   In the folder's **Security** tab, verify that **Everyone** (or **Users**) has
   **Read & execute**, **List folder contents**, and **Read** permissions. Share
   and filesystem permissions both apply.

3. **Add the share in PKG Manager.** In **Settings → Samba**, enter the PC's IP
   address as **Server**, leave port **445**, and enter `PS5PKG` as **Share**.
   Leave **Username** and **Password** blank. Use **Test connection**, then save
   the share. If **Select share** fails, type the share name directly: Windows
   may block listing shares while allowing access to a known share.

<details>
<summary><b>Troubleshooting / If connection fails (Windows 11 24H2, Pro, or Enterprise)</b></summary>

Newer Windows 11 releases (such as 24H2) or Pro/Enterprise editions enforce SMB
signing and restrict guest logons by default. If the connection fails or
returns access denied errors, check the following:

#### 1. Enable the built-in Guest account
Open **PowerShell as Administrator** and run:

```powershell
$guest = Get-LocalUser | Where-Object { $_.SID.Value -like '*-501' }
$guest | Enable-LocalUser
$guest | Select-Object Name
```

#### 2. Permit Guest to log on over the network
On Windows editions with Local Security Policy, open `secpol.msc` and navigate
to **Local Policies → User Rights Assignment**:
* Remove **Guest** from **Deny access to this computer from the network**.
* Ensure **Access this computer from the network** includes **Guest** or a group
  that permits its access.

#### 3. Allow unsigned guest sessions and disable encryption
Windows guest sessions cannot use SMB signing or encryption. In administrator
PowerShell, run:

```powershell
# Disable signing requirement for the SMB server:
Set-SmbServerConfiguration -RequireSecuritySignature $false -Force

# Verify and disable SMB encryption if enabled:
Set-SmbServerConfiguration -EncryptData $false -Force
Set-SmbShare -Name 'PS5PKG' -EncryptData $false -Force
```

#### Common Failures

| Error | What to check |
| --- | --- |
| Account disabled / `0xC0000072` | Enable the Guest account in step 1 above. |
| Logon type not granted / `0xC000015B` | Check the network logon policies in `secpol.msc` (step 2). |
| Signing required | Disable the server signing requirement (step 3), or use a Windows account with credentials. |
| Access denied / `0xC0000022` | Check guest logon, signing/encryption requirements, and both Share and Security permission lists. |

> **Note**: Windows' **Enable insecure guest logons** (`AllowInsecureGuestAuth`)
> policy controls Windows acting as an SMB *client*. It does not enable guest
> access to shares *hosted* by that PC.

References: Microsoft's [Windows file-sharing guide](https://support.microsoft.com/en-us/windows/experience/connectivity-networking/file-sharing-over-a-network-in-windows),
[SMB signing requirements](https://learn.microsoft.com/en-us/windows-server/storage/file-server/smb-signing),
[Guest account activation](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.localaccounts/enable-localuser),
and [network logon deny policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/deny-access-to-this-computer-from-the-network).
For diagnostic commands and Windows VM test results, see
[SMB diagnostics](docs/SMB_DIAGNOSTICS.md).

</details>

## Architecture
For in-depth technical details regarding the system architecture, range streaming, and installation pipeline, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Credits
The following projects were used as foundations or reference for different parts of this project:
- [John Törnblom](https://github.com/john-tornblom) - [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)
- [LightningMods](https://github.com/LightningMods) - [etaHEN](https://github.com/etaHEN/etaHEN)
- [earthonion](https://github.com/earthonion) - [garlic-savemgr](https://github.com/earthonion/garlic-savemgr)
- [sahlberg](https://github.com/sahlberg) - [libsmb2](https://github.com/sahlberg/libsmb2)
- Everyone contributing to the PS5 homebrew scene.

## Donations
If you'd like to support my work, please check out [DONATE.md](DONATE.md).

## Development
For build instructions, test runner details, and deployment scripts, see [DEVELOPMENT.md](DEVELOPMENT.md).
