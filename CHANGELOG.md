# Changelog

## v1.2.3

### Startup Reliability
- Fixed startup failures affecting some users by making process discovery safer across PS5 environments, improving initialization ordering, and adding earlier diagnostics and notifications when startup steps fail

### Installation
- Fixed the Base + Update handoff so the update is queued by the native installer and continues even if the browser is closed after starting the base installation

### SMB Network Shares
- Added guided share selection and folder browsing, so share names and paths no longer need to be entered manually
- Preserve saved credentials when browsing an edited share whose password is masked in the UI, avoiding accidental guest logins
- Treat SMB sources consistently as read-only without attempting a network write-permission probe

### Browser Navigation
- Fixed browser Back/Forward navigation so returning to the app after visiting another page preserves the app's main page and menu history

---

## v1.2.2

### Package Detection
- Fixed PS4 base packages being incorrectly identified as DLC

### Cache and Scanning
- Cache is cleared and the package catalog is rescanned automatically after an app update; clearing the cache manually also refreshes the catalog

---

## v1.2.1

### Direct Install
- PKG files can now be installed from another device, such as a PC, directly to the console without a temporary disk copy

### Networking
- Moved package streaming and Direct Install services to ports `18841` and `18842` to avoid potential conflicts with other homebrew applications. The web interface remains on port `8844`.

### SMB Streaming
- Improved SMB package streaming performance, reaching approximately 110 MB/s during installs in ideal conditions
- Added streaming diagnostics to help identify network and server bottlenecks

### Extended USB Storage
- Available-space checks now include extended USB storage (`/mnt/ext0`)

### Package Badges
- PS4 and PS5 package badges are now shown in package lists

### Package Detection and Scanning
- Fixed detection of PS5 DLC packages
- Fixed drive scans hiding other packages when an unknown package was present, such as during a copy operation

---

## v1.1.0

### Multi-Language PKG Titles
- Package titles now display in your browser's preferred language instead of the first available entry

### Controller Navigation (PS5 Browser)
- **Circle button** navigates back through pages instead of immediately closing the browser
- Pressing Circle on the root Drive Select screen or during installation still closes the browser
- Modals block spatial navigation in the background while open

### SMB Diagnostics
- SMB connection testing now shows clearer, actionable error messages — making it easier to diagnose Windows share issues

### Storage Display
- The header now shows free space for **Internal** and **M.2** drives separately
- Installation is blocked when there isn't enough free space on any available drive

---

## v1.0.0

- Initial Release
