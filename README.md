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
- **Reliable Repeat Installs**: Runs each package installation in a fresh helper process while the manager stays open.

## Installation

Download the latest versioned ELF (for example, `pkg-manager_v1.3.0.elf`) from the [Releases](https://github.com/itsPLK/ps5-pkg-manager/releases) page.

- **Payload Manager (Recommended)**: Use [Payload Manager](https://github.com/itsPLK/ps5-payload-manager) to launch the downloaded ELF automatically.
- **Manual ELF Loading**: You can load the downloaded ELF like any other standard ELF payload.

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
You can configure SMB network shares in the app's **Settings** tab to browse and install packages stored on your PC or NAS.

For large shares, enable **Browse only** when adding or editing a share to skip
full and background catalog scans. Open the share from the storage screen or
choose **Browse files** in Samba settings, navigate folders, select a PKG, and
choose **Install selected PKG**. Folder listings have 64 entries per page;
metadata is read only for the selected file. The scanned catalog shows 60 titles
per page.

Full rescans run in the background. Retrying or reopening the interface attaches
to an active scan without queuing another pass. Network shares are not rescanned
by the frontend's 15-second polling timer; use **Rescan** to refresh their catalog.

### Direct Install
From another device on the same network, open the PKG Manager interface and choose **Direct Install**. Select or drop a local `.pkg` file to install it directly on the console.

### Multi-Part Packages
If you want to back up large packages onto optical discs (Blu-ray, DVD) or are limited by storage media size, you can split your package into multi-part files using the included tool:

```bash
python3 tools/pkg_split.py /path/to/package.pkg -s 23G
```

Multi-part packages can be burned across multiple discs or loaded directly from a USB drive. When installing from discs, the installer will automatically detect inserted media and prompt you with on-screen notifications whenever a disc swap is needed.

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
