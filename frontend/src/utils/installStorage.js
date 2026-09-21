export function getTitlePlatform(titleId) {
  const normalizedTitleId = (titleId || '').trim().toUpperCase();
  if (normalizedTitleId.startsWith('PPSA')) return 'PS5';
  if (normalizedTitleId.startsWith('CUSA')) return 'PS4';
  return null;
}
export function getInstallStorageOptions(storage, titleId) {
  if (!storage) return [];

  const options = [{
    label: 'Internal',
    free: storage.internal?.free ?? storage.free ?? 0
  }];

  if (storage.nvme?.available) {
    options.push({ label: 'M.2 NVMe', free: storage.nvme.free ?? 0 });
  }

  // USB extended storage can install PS4 titles, but never PS5 titles.
  if (getTitlePlatform(titleId) === 'PS4' && storage.usb?.available) {
    options.push({ label: 'USB', free: storage.usb.free ?? 0 });
  }

  return options;
}
