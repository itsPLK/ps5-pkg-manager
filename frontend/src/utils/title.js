import { BUILD_VERSION, BUILD_COMMIT, BUILD_DATE, isPlayStation } from '../constants/config.js';
import { formatBytes, formatEta } from './formatters.js';

const initialDocTitle = typeof document !== 'undefined' ? document.title : '';

export function getBrowserTitle(ver) {
  const raw = ver || BUILD_VERSION;
  const v = String(raw).trim().replace(/^v+/i, '') || BUILD_VERSION;
  if (BUILD_COMMIT && BUILD_DATE) {
    return `Internal PKG Manager v${v} (${BUILD_COMMIT}, ${BUILD_DATE}) by PLK`;
  }
  const titleToCheck = initialDocTitle || (typeof document !== 'undefined' ? document.title : '');
  if (titleToCheck) {
    const match = titleToCheck.match(/\(([^,]+),\s*([^)]+)\)/);
    if (match) {
      return `Internal PKG Manager v${v} (${match[1]}, ${match[2]}) by PLK`;
    }
  }
  return `Internal PKG Manager v${v} by PLK`;
}

export function makeTextProgressBar(percent, length = 10) {
  const p = Math.min(100, Math.max(0, percent || 0));
  const filled = Math.min(length, Math.max(0, Math.round((p / 100) * length)));
  const empty = length - filled;
  return `[${'■'.repeat(filled)}${'□'.repeat(empty)}]`;
}

export function formatInstallTitle({
  activeJob,
  totalCount = 1,
  currentIndex = 1,
  speed = 0,
  appVersion,
  onPS5 = isPlayStation,
}) {
  const defaultTitle = getBrowserTitle(appVersion);
  if (!activeJob) return defaultTitle;

  const isInstalling = activeJob.state === 'installing';
  const isPreparing = activeJob.state === 'preparing';
  if (!isInstalling && !isPreparing) return defaultTitle;

  // On non-PS5 desktop browsers, keep compact percentage in title
  if (!onPS5) {
    if (activeJob.is_direct_storage) return `(Direct Storage) ${defaultTitle}`;
    if (isPreparing) return `(Preparing) ${defaultTitle}`;
    const pct = Math.floor(Math.min(100, Math.max(0, activeJob.progress || 0)));
    return `(${pct}%) ${defaultTitle}`;
  }

  // On PS5: detailed title with pseudo text-based progress bar
  let titleName = activeJob.title_name || (activeJob.path ? activeJob.path.split('/').pop() : '') || 'Package';
  if (titleName.length > 40) {
    titleName = `${titleName.slice(0, 37)}...`;
  }
  const batchPrefix = totalCount > 1 ? `[${currentIndex}/${totalCount}] ` : '';

  if (activeJob.is_direct_storage) {
    return `[Direct Storage] ${batchPrefix}${titleName}`.trim();
  }
  if (isPreparing) {
    return `[Preparing] ${batchPrefix}${titleName}`.trim();
  }

  const pct = Math.floor(Math.min(100, Math.max(0, activeJob.progress || 0)));
  const bar = makeTextProgressBar(pct, 10);

  const parts = [`${bar} ${pct}%`];
  parts.push(`${batchPrefix}${titleName}`.trim());

  if (speed > 0) {
    parts.push(`${formatBytes(speed)}/s`);
    const remainingBytes = Math.max(0, (activeJob.total_bytes || 0) - (activeJob.downloaded_bytes || 0));
    if (remainingBytes > 0) {
      const eta = formatEta(remainingBytes / speed);
      if (eta) parts.push(eta);
    }
  } else if (activeJob.total_bytes > 0 && activeJob.downloaded_bytes > 0) {
    parts.push(`${formatBytes(activeJob.downloaded_bytes)} / ${formatBytes(activeJob.total_bytes)}`);
  }

  return parts.join(' · ');
}

export function getFullVersion(ver) {
  const raw = ver || BUILD_VERSION;
  const v = String(raw).trim().replace(/^v+/i, '') || BUILD_VERSION;
  if (BUILD_COMMIT && BUILD_DATE) {
    return `Internal PKG Manager v${v} (${BUILD_COMMIT}, ${BUILD_DATE})`;
  }
  if (typeof document !== 'undefined' && document.title) {
    const match = document.title.match(/\(([^,]+),\s*([^)]+)\)/);
    if (match) {
      return `Internal PKG Manager v${v} (${match[1]}, ${match[2]})`;
    }
  }
  if (BUILD_COMMIT) {
    return `Internal PKG Manager v${v} (${BUILD_COMMIT})`;
  }
  return `Internal PKG Manager v${v}`;
}

export function getLocalizedTitle(pkg) {
  if (!pkg) return '';
  let loc = pkg.localized_titles;
  if (typeof loc === 'string' && loc.trim().startsWith('{')) {
    try { loc = JSON.parse(loc); } catch (e) { loc = null; }
  }
  if (!loc || typeof loc !== 'object' || Object.keys(loc).length === 0) {
    return pkg.title_name || '';
  }

  const navLangs = (typeof navigator !== 'undefined' && Array.isArray(navigator.languages) && navigator.languages.length > 0)
    ? navigator.languages
    : [(typeof navigator !== 'undefined' && (navigator.language || navigator.userLanguage)) || 'en-US'];

  // 1. Exact match in browser languages (e.g. 'en-US', 'ar-AE', 'ru-RU')
  for (const l of navLangs) {
    if (!l) continue;
    const cleanL = l.trim().toLowerCase();
    for (const [k, v] of Object.entries(loc)) {
      if (k.toLowerCase() === cleanL && v) return v;
    }
  }

  // 2. Primary language match (e.g. 'en' matches 'en-US', 'ar' matches 'ar-AE', 'ru' matches 'ru-RU')
  for (const l of navLangs) {
    if (!l) continue;
    const primary = l.split('-')[0].split('_')[0].toLowerCase();
    for (const [k, v] of Object.entries(loc)) {
      const kPrimary = k.split('-')[0].split('_')[0].toLowerCase();
      if (kPrimary === primary && v) return v;
    }
  }

  // 3. Fallback to default_language from package (e.g. 'en-US')
  if (pkg.default_language) {
    const defLang = pkg.default_language.trim().toLowerCase();
    for (const [k, v] of Object.entries(loc)) {
      if (k.toLowerCase() === defLang && v) return v;
    }
    const defPrimary = defLang.split('-')[0].split('_')[0];
    for (const [k, v] of Object.entries(loc)) {
      if (k.split('-')[0].split('_')[0].toLowerCase() === defPrimary && v) return v;
    }
  }

  // 4. Fallback to English
  for (const [k, v] of Object.entries(loc)) {
    if (k.toLowerCase().startsWith('en') && v) return v;
  }

  // 5. Fallback to existing title_name if not generic
  if (pkg.title_name && pkg.title_name !== 'Unknown Package' && pkg.title_name !== 'Package') {
    return pkg.title_name;
  }

  // 6. First localized title
  const first = Object.values(loc)[0];
  return first || pkg.title_name || '';
}
