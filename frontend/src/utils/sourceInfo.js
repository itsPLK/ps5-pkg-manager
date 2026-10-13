export function getSourceInfo(pkgPath, drivesList = []) {
  if (!pkgPath) return { id: 'unknown', name: 'Unknown', type: 'unknown' };

  // Check USB: /mnt/usb0 .. /mnt/usb7
  const usbMatch = pkgPath.match(/^\/mnt\/usb(\d+)/i);
  if (usbMatch) {
    const num = usbMatch[1];
    return {
      id: `usb${num}`,
      name: `USB${num}`,
      type: 'usb'
    };
  }

  // Check Disc: /mnt/disc
  if (pkgPath.match(/^\/mnt\/disc/i)) {
    return {
      id: 'disc',
      name: 'Disc',
      type: 'disc'
    };
  }

  // Check Internal storage: /data/pkg
  if (pkgPath.match(/^\/data\/pkg(\/|$)/i)) {
    return {
      id: 'internal',
      name: 'Internal',
      type: 'internal'
    };
  }

  // Check SMB: smb://...
  if (pkgPath.startsWith('smb://')) {
    let matchedDrive = null;
    if (Array.isArray(drivesList)) {
      const matching = drivesList.filter((d) => {
        if (d.type !== 'smb' || !d.path) return false;
        const normDPath = d.path.replace(/\/+$/, '');
        const normPkg = pkgPath.replace(/\/+$/, '');
        return normPkg === normDPath || normPkg.startsWith(normDPath + '/');
      });
      matching.sort((a, b) => (b.path || '').length - (a.path || '').length);
      matchedDrive = matching[0] || null;
    }

    let rawName = '';
    if (matchedDrive) {
      rawName = (matchedDrive.label || '').replace(/^SMB:\s*/i, '').trim();
    }

    if (!rawName) {
      try {
        const withoutProto = pkgPath.replace(/^smb:\/\//, '');
        const parts = withoutProto.split('/');
        if (parts.length > 1 && parts[1]) {
          rawName = decodeURIComponent(parts[1]);
        } else if (parts.length > 0 && parts[0]) {
          rawName = decodeURIComponent(parts[0]);
        }
      } catch (e) {}
    }

    if (!rawName) rawName = 'SMB';

    const MAX_SMB_LEN = 11;
    let displayName = rawName;
    if (displayName.length > MAX_SMB_LEN) {
      displayName = displayName.slice(0, MAX_SMB_LEN - 3).trim() + '...';
    }

    const driveId = matchedDrive && matchedDrive.id ? matchedDrive.id : `smb_${rawName.toLowerCase()}`;
    return {
      id: driveId,
      name: displayName,
      type: 'smb'
    };
  }

  const mntMatch = pkgPath.match(/^\/mnt\/([^\/]+)/i);
  if (mntMatch) {
    let name = mntMatch[1].toUpperCase();
    if (name.length > 11) name = name.slice(0, 8) + '...';
    return { id: mntMatch[1].toLowerCase(), name, type: 'other' };
  }

  return { id: 'other', name: 'Other', type: 'other' };
}
