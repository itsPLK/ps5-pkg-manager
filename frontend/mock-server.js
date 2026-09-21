import http from 'http';
import crypto from 'crypto';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const PORT = 8844;

// 14 fictional, original packages spanning USB, Disc, and two Samba shares
const examplePkgs = [
  // --- USB Drive 0 (/mnt/usb0) ---
  {
    path: '/mnt/usb0/Astraea_Veil_of_Cygnus_v01.000.pkg',
    filename: 'Astraea_Veil_of_Cygnus_v01.000.pkg',
    title_id: 'PPSA01001',
    title_name: 'Astraea: Veil of Cygnus',
    content_id: 'EP9000-PPSA01001_00-ASTRAEACYGNUS001',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1718000000,
    file_size: 48318382080,
    total_pkg_size: 48318382080,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },
  {
    path: '/mnt/usb0/Astraea_Veil_of_Cygnus_Patch_v01.040.pkg',
    filename: 'Astraea_Veil_of_Cygnus_Patch_v01.040.pkg',
    title_id: 'PPSA01001',
    title_name: 'Astraea: Veil of Cygnus',
    content_id: 'EP9000-PPSA01001_00-ASTRAEACYGNUS001',
    app_version: '01.040.000',
    pkg_type: 'update',
    category: 'gp',
    mtime: 1719500000,
    file_size: 12884901888,
    total_pkg_size: 12884901888,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Base package is not installed',
    blurhash: ''
  },
  {
    path: '/mnt/usb0/Astraea_Outer_Rim_Expansion.pkg',
    filename: 'Astraea_Outer_Rim_Expansion.pkg',
    title_id: 'PPSA01001',
    title_name: 'Astraea: Outer Rim Expansion',
    content_id: 'EP9000-PPSA01001_00-OUTERRIMEXPANS01',
    app_version: '01.000.000',
    pkg_type: 'dlc',
    category: 'ac',
    mtime: 1719000000,
    file_size: 16106127360,
    total_pkg_size: 16106127360,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Base package is not installed',
    blurhash: ''
  },
  {
    path: '/mnt/usb0/UmbraTrace_Cyber_Protocol_Base.pkg',
    filename: 'UmbraTrace_Cyber_Protocol_Base.pkg',
    title_id: 'PPSA01003',
    title_name: 'UmbraTrace: Cyber Protocol',
    content_id: 'EP9000-PPSA01003_00-UMBRATRACE000001',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1715000000,
    file_size: 64424509440,
    total_pkg_size: 64424509440,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: true,
    installed_version: '01.000.000',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Application is already installed',
    blurhash: ''
  },
  {
    path: '/mnt/usb0/UmbraTrace_Patch_v02.100.pkg',
    filename: 'UmbraTrace_Patch_v02.100.pkg',
    title_id: 'PPSA01003',
    title_name: 'UmbraTrace: Cyber Protocol',
    content_id: 'EP9000-PPSA01003_00-UMBRATRACE000001',
    app_version: '02.100.000',
    pkg_type: 'update',
    category: 'gp',
    mtime: 1719200000,
    file_size: 18253611008,
    total_pkg_size: 18253611008,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: true,
    installed_version: '01.000.000',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },
  {
    path: '/mnt/usb0/Oscillovox_Studio_v01.000.pkg',
    filename: 'Oscillovox_Studio_v01.000.pkg',
    title_id: 'PPSA01005',
    title_name: 'Oscillovox Studio',
    content_id: 'EP9000-PPSA01005_00-OSCILLOVOXSTUD01',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1716000000,
    file_size: 14856000000,
    total_pkg_size: 14856000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },

  // --- Optical Blu-ray Disc (/mnt/disc) - Multi-part ---
  {
    path: '/mnt/disc/pkg/Solvanna_Sands_of_Aeon.pkg.part1',
    filename: 'Solvanna_Sands_of_Aeon.pkg.part1',
    title_id: 'PPSA01002',
    title_name: 'Solvanna: Sands of Aeon',
    content_id: 'EP9000-PPSA01002_00-SOLVANNAAEON0001',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1717500000,
    file_size: 47100000000,
    total_pkg_size: 94200000000,
    has_icon: true,
    is_multipart: true,
    part_index: 1,
    total_parts: 2,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },

  // --- SMB Share 1: Games (smb://192.168.1.100/Games) ---
  {
    path: 'smb://192.168.1.100/Games/Vespergarde_Skyward_Realms_Base.pkg',
    filename: 'Vespergarde_Skyward_Realms_Base.pkg',
    title_id: 'PPSA01004',
    title_name: 'Vespergarde: Skyward Realms',
    content_id: 'EP9000-PPSA01004_00-VESPERGARDE00001',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1714000000,
    file_size: 52800000000,
    total_pkg_size: 52800000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: true,
    installed_version: '01.000.000',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Application is already installed',
    blurhash: ''
  },
  {
    path: 'smb://192.168.1.100/Games/Vespergarde_Shards_of_Dawn.pkg',
    filename: 'Vespergarde_Shards_of_Dawn.pkg',
    title_id: 'PPSA01004',
    title_name: 'Vespergarde: Shards of Dawn',
    content_id: 'EP9000-PPSA01004_00-SHARDSOFDAWN0001',
    app_version: '01.000.000',
    pkg_type: 'dlc',
    category: 'ac',
    mtime: 1718500000,
    file_size: 22400000000,
    total_pkg_size: 22400000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },
  {
    path: 'smb://192.168.1.100/Games/PolyStratix_Engine.pkg',
    filename: 'PolyStratix_Engine.pkg',
    title_id: 'PPSA01006',
    title_name: 'PolyStratix Engine',
    content_id: 'EP9000-PPSA01006_00-POLYSTRATIXENG01',
    app_version: '01.000.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1716500000,
    file_size: 36400000000,
    total_pkg_size: 36400000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },
  {
    path: 'smb://192.168.1.100/Games/PolyStratix_Asset_Pack_Vol1.pkg',
    filename: 'PolyStratix_Asset_Pack_Vol1.pkg',
    title_id: 'PPSA01006',
    title_name: 'PolyStratix: Asset Pack Vol. 1',
    content_id: 'EP9000-PPSA01006_00-ASSETPACKVOL0001',
    app_version: '01.000.000',
    pkg_type: 'dlc',
    category: 'ac',
    mtime: 1716600000,
    file_size: 8200000000,
    total_pkg_size: 8200000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Base package is not installed',
    blurhash: ''
  },

  // --- SMB Share 2: Homebrew (smb://nas.local/Homebrew) ---
  {
    path: 'smb://nas.local/Homebrew/PolyArcadia_PS5_v01.180.pkg',
    filename: 'PolyArcadia_PS5_v01.180.pkg',
    title_id: 'HB0000001',
    title_name: 'PolyArcadia PS5',
    content_id: 'HB0000-HB0000001_00-POLYARCADIA00001',
    app_version: '01.180.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1718500000,
    file_size: 485000000,
    total_pkg_size: 485000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: true,
    install_disabled_reason: '',
    blurhash: ''
  },
  {
    path: 'smb://nas.local/Homebrew/VortiView_Media_Player.pkg',
    filename: 'VortiView_Media_Player.pkg',
    title_id: 'HB0000002',
    title_name: 'VortiView Media Player',
    content_id: 'HB0000-HB0000002_00-VORTIVIEW0000001',
    app_version: '02.400.000',
    pkg_type: 'base',
    category: 'gd',
    mtime: 1719100000,
    file_size: 124000000,
    total_pkg_size: 124000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: true,
    installed_version: '02.400.000',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Application is already installed',
    blurhash: ''
  },
  {
    path: 'smb://nas.local/Homebrew/Oscillovox_Pro_Synth_Pack.pkg',
    filename: 'Oscillovox_Pro_Synth_Pack.pkg',
    title_id: 'PPSA01005',
    title_name: 'Oscillovox: Pro Synth Pack',
    content_id: 'EP9000-PPSA01005_00-PROSYNTHPACK0001',
    app_version: '01.000.000',
    pkg_type: 'dlc',
    category: 'ac',
    mtime: 1717000000,
    file_size: 4500000000,
    total_pkg_size: 4500000000,
    has_icon: true,
    is_multipart: false,
    part_index: 0,
    total_parts: 0,
    is_installed: false,
    installed_version: '',
    is_dlc_installed: false,
    has_leftover: false,
    leftover_desc: '',
    is_partially_installed: false,
    partial_desc: '',
    can_install: false,
    install_disabled_reason: 'Base package is not installed',
    blurhash: ''
  }
];

const mockDrives = [
  {
    id: 'usb0',
    type: 'usb',
    label: 'USB Drive 0',
    path: '/mnt/usb0',
    mounted: true,
    pkg_count: 6,
    clickable: true
  },
  {
    id: 'disc',
    type: 'disc',
    label: 'Blu-ray Disc',
    path: '/mnt/disc',
    mounted: true,
    pkg_count: 1,
    clickable: true
  },
  {
    id: 'smb_games',
    type: 'smb',
    label: 'SMB: Games',
    path: 'smb://192.168.1.100/Games',
    mounted: true,
    pkg_count: 4,
    clickable: true
  },
  {
    id: 'smb_homebrew',
    type: 'smb',
    label: 'SMB: Homebrew',
    path: 'smb://nas.local/Homebrew',
    mounted: true,
    pkg_count: 3,
    clickable: true
  }
];

const mockStorage = {
  free: 412582000000,
  total: 667200000000,
  used: 254618000000,
  path: '/data',
  label: 'Internal',
  internal: {
    free: 412582000000,
    total: 667200000000,
    used: 254618000000,
    path: '/data',
    label: 'Internal'
  },
  nvme: {
    available: true,
    free: 1425890000000,
    total: 2000398000000,
    used: 574508000000,
    path: '/mnt/ext1',
    label: 'M.2 NVMe'
  }
};

const mockSettings = {
  move_installed_to_end: true,
  fade_installed_packages: true,
  all_sources_mode: true,
  smb_shares: [
    {
      id: 'smb_games',
      name: 'Main NAS - Games',
      server: '192.168.1.100',
      share: 'Games',
      path: '',
      username: 'guest',
      workgroup: 'WORKGROUP',
      enabled: true
    },
    {
      id: 'smb_homebrew',
      name: 'Homebrew Share',
      server: 'nas.local',
      share: 'Homebrew',
      path: '',
      username: 'media',
      workgroup: 'WORKGROUP',
      enabled: true
    }
  ]
};

// Clean abstract SVG fallback without any text, logos, studio names, or corner badges
function generateCleanSvgFallback(titleId) {
  const hues = [210, 260, 330, 160, 35, 190, 280, 120];
  let sum = 0;
  for (let i = 0; i < titleId.length; i++) sum += titleId.charCodeAt(i);
  const hue1 = hues[sum % hues.length];
  const hue2 = (hue1 + 60) % 360;

  return `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">
  <defs>
    <linearGradient id="bgGrad" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="hsl(${hue1}, 75%, 16%)"/>
      <stop offset="50%" stop-color="hsl(${hue1}, 65%, 28%)"/>
      <stop offset="100%" stop-color="hsl(${hue2}, 85%, 45%)"/>
    </linearGradient>
    <radialGradient id="sun" cx="50%" cy="50%" r="50%">
      <stop offset="0%" stop-color="#ffffff" stop-opacity="0.3"/>
      <stop offset="100%" stop-color="#000000" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect width="512" height="512" fill="url(#bgGrad)"/>
  <circle cx="256" cy="256" r="180" fill="url(#sun)"/>
  <path d="M60 380 Q 256 220 452 380 L 452 512 L 60 512 Z" fill="#030712" fill-opacity="0.6"/>
  <path d="M0 430 Q 256 310 512 430 L 512 512 L 0 512 Z" fill="#020408" fill-opacity="0.85"/>
</svg>`;
}

let installerStatus = {
  is_installing: false,
  pkg_path: '',
  title_id: '',
  title_name: '',
  content_id: '',
  status: 'idle',
  downloaded_bytes: 0,
  total_bytes: 0,
  progress: 0,
  error_code: 0,
  completed: false,
  failed: false,
  is_multipart: false,
  current_part: 0,
  total_parts: 0,
  waiting_for_disc: false,
  prompt_message: ''
};

let installInterval = null;

// Direct-install mock session (memory-backed like the device RAM ring).
const WS_MOCK_PORT = 8846;
const mockUpload = { active: false, id: '', owner: '', filename: '', total: 0, received: 0, buf: null, icon: null };

function setCors(res) {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
}

const server = http.createServer(async (req, res) => {
  setCors(res);

  if (req.method === 'OPTIONS') {
    res.writeHead(200);
    res.end();
    return;
  }

  const parsedUrl = new URL(req.url, `http://127.0.0.1:${PORT}`);
  const pathname = parsedUrl.pathname;

  console.log(`[Mock Server] ${req.method} ${pathname}`);

  // 1. Drives API
  if (req.method === 'GET' && pathname === '/api/drives') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockDrives));
    return;
  }

  // 2. Storage Space API
  if (req.method === 'GET' && pathname === '/api/storage') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockStorage));
    return;
  }

  // 3. Packages List API (with smart drive filtering)
  if (req.method === 'GET' && pathname === '/api/packages') {
    const driveParam = parsedUrl.searchParams.get('drive');
    let pkgs = examplePkgs.filter(p => !p.filename.startsWith('.'));

    if (driveParam && driveParam !== '__all__') {
      const dp = decodeURIComponent(driveParam);
      if (dp === 'usb0' || dp === '/mnt/usb0') {
        pkgs = examplePkgs.filter(p => p.path.startsWith('/mnt/usb0'));
      } else if (dp === 'disc' || dp === '/mnt/disc') {
        pkgs = examplePkgs.filter(p => p.path.startsWith('/mnt/disc'));
      } else if (dp === 'smb_games' || dp.includes('192.168.1.100') || dp.toLowerCase().includes('games')) {
        pkgs = examplePkgs.filter(p => p.path.startsWith('smb://192.168.1.100/Games'));
      } else if (dp === 'smb_homebrew' || dp.includes('nas.local') || dp.toLowerCase().includes('homebrew')) {
        pkgs = examplePkgs.filter(p => p.path.startsWith('smb://nas.local/Homebrew'));
      } else {
        pkgs = examplePkgs.filter(p => p.path.startsWith('/mnt/' + dp) || p.path.startsWith('smb://' + dp));
      }
    }

    const acceptLang = req.headers['accept-language'] || '';
    const outputPkgs = pkgs.map(p => ({
      localized_titles: {},
      default_language: '',
      ...p
    }));

    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(outputPkgs));
    return;
  }

  // 4. Quick Scan & Refresh APIs
  if (req.method === 'POST' && (pathname === '/api/packages/quick-scan' || (pathname === '/api/packages/refresh' && parsedUrl.searchParams.get('quick') !== null))) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', count: examplePkgs.length, changed: false }));
    return;
  }

  if (req.method === 'POST' && pathname === '/api/packages/refresh') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', count: examplePkgs.length }));
    return;
  }

  // 5. High-Resolution Clean Cover Art API (No text, no tags, no badges)
  if ((req.method === 'GET' || req.method === 'HEAD') && pathname === '/api/icon') {
    const pkgPath = parsedUrl.searchParams.get('path');
    if (pkgPath?.startsWith('live:')) {
      if (mockUpload.active && pkgPath === 'live:' + mockUpload.id && mockUpload.icon) {
        res.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'no-store' });
        res.end(req.method === 'HEAD' ? undefined : mockUpload.icon);
      } else {
        res.writeHead(404);
        res.end();
      }
      return;
    }
    const pkg = examplePkgs.find(p => p.path === pkgPath) || {
      title_id: 'PPSA01001'
    };

    // Check for high-res image in frontend/public/mock-icons/<title_id>.jpg or .png
    const jpgIcon = path.resolve(__dirname, 'public/mock-icons', `${pkg.title_id}.jpg`);
    const pngIcon = path.resolve(__dirname, 'public/mock-icons', `${pkg.title_id}.png`);

    if (fs.existsSync(jpgIcon)) {
      res.writeHead(200, { 'Content-Type': 'image/jpeg', 'Cache-Control': 'public, max-age=86400' });
      if (req.method === 'HEAD') {
        res.end();
      } else {
        fs.createReadStream(jpgIcon).pipe(res);
      }
      return;
    }

    if (fs.existsSync(pngIcon)) {
      res.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'public, max-age=86400' });
      if (req.method === 'HEAD') {
        res.end();
      } else {
        fs.createReadStream(pngIcon).pipe(res);
      }
      return;
    }

    // Clean fallback SVG with no text or badges
    const svg = generateCleanSvgFallback(pkg.title_id || 'PPSA01001');
    res.writeHead(200, { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'public, max-age=86400' });
    if (req.method === 'HEAD') {
      res.end();
    } else {
      res.end(svg);
    }
    return;
  }

  // 6. Installation Simulation API
  if (req.method === 'POST' && pathname === '/api/install') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        const { path: targetPath } = JSON.parse(body || '{}');
        const pkg = examplePkgs.find(p => p.path === targetPath) || {
          path: targetPath,
          title_id: 'PPSA01001',
          title_name: targetPath && targetPath.startsWith('live:') ? mockUpload.filename || 'Live upload' : 'Package',
          content_id: 'EP0000-PPSA01001_00-APP',
          file_size: targetPath && targetPath.startsWith('live:') && mockUpload.total > 0 ? mockUpload.total : 100000000
        };

        if (installerStatus.is_installing) {
          res.writeHead(409, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ success: false, error: 'Installation already in progress' }));
          return;
        }

        const isMulti = !!pkg.is_multipart;
        installerStatus = {
          is_installing: true,
          pkg_path: pkg.path,
          title_id: pkg.title_id,
          title_name: pkg.title_name,
          content_id: pkg.content_id,
          status: isMulti ? 'copying' : 'transferring',
          downloaded_bytes: 0,
          total_bytes: pkg.total_pkg_size || pkg.file_size,
          progress: 0,
          error_code: 0,
          completed: false,
          failed: false,
          is_multipart: isMulti,
          current_part: isMulti ? 1 : 0,
          total_parts: isMulti ? (pkg.total_parts || 2) : 0,
          waiting_for_disc: false,
          prompt_message: isMulti ? `Copying Part 1 of ${pkg.total_parts || 2} from disc...` : 'Installing package...'
        };

        if (installInterval) clearInterval(installInterval);

        if (isMulti) {
          let step = 0;
          installInterval = setInterval(() => {
            if (!installerStatus.is_installing) {
              clearInterval(installInterval);
              return;
            }
            step++;
            if (step <= 3) {
              installerStatus.status = 'copying';
              installerStatus.current_part = 1;
              installerStatus.waiting_for_disc = false;
              installerStatus.prompt_message = `Copying Part 1 of ${installerStatus.total_parts} from disc...`;
              installerStatus.progress = step * 10;
              installerStatus.downloaded_bytes = Math.floor((installerStatus.total_bytes / 3) * (step / 3));
            } else if (step === 4) {
              installerStatus.status = 'waiting_disc';
              installerStatus.current_part = 2;
              installerStatus.waiting_for_disc = true;
              installerStatus.prompt_message = `Please insert Disc 2 of ${installerStatus.total_parts}`;
            } else if (step <= 7) {
              installerStatus.status = 'copying';
              installerStatus.current_part = 2;
              installerStatus.waiting_for_disc = false;
              installerStatus.prompt_message = `Copying Part 2 of ${installerStatus.total_parts} from disc...`;
              installerStatus.progress = 30 + (step - 4) * 10;
              installerStatus.downloaded_bytes = Math.floor(installerStatus.total_bytes * (installerStatus.progress / 100));
            } else if (step <= 9) {
              installerStatus.status = 'transferring';
              installerStatus.waiting_for_disc = false;
              installerStatus.prompt_message = 'Finalizing package installation...';
              installerStatus.progress = 70 + (step - 7) * 12;
            } else {
              installerStatus.progress = 100;
              installerStatus.status = 'playable';
              installerStatus.is_installing = false;
              installerStatus.completed = true;
              installerStatus.prompt_message = '';
              clearInterval(installInterval);
            }
          }, 1000);
        } else {
          installInterval = setInterval(() => {
            if (!installerStatus.is_installing) {
              clearInterval(installInterval);
              return;
            }
            installerStatus.downloaded_bytes += Math.floor(installerStatus.total_bytes / 10);
            if (installerStatus.downloaded_bytes >= installerStatus.total_bytes) {
              installerStatus.downloaded_bytes = installerStatus.total_bytes;
              installerStatus.progress = 100;
              installerStatus.status = 'playable';
              installerStatus.is_installing = false;
              installerStatus.completed = true;
              clearInterval(installInterval);
            } else {
              installerStatus.progress = Math.round((installerStatus.downloaded_bytes / installerStatus.total_bytes) * 100);
            }
          }, 400);
        }

        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: true, message: 'Installation started' }));
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, error: e.message }));
      }
    });
    return;
  }

  // 7. Cancel Install API
  if (req.method === 'POST' && pathname === '/api/cancel') {
    if (installInterval) {
      clearInterval(installInterval);
      installInterval = null;
    }
    installerStatus.is_installing = false;
    installerStatus.failed = true;
    installerStatus.status = 'canceled';
    installerStatus.prompt_message = 'Installation was canceled';
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ success: true, message: 'Installation canceled' }));
    return;
  }

  // 8. Polling / Status API
  if (req.method === 'GET' && (pathname === '/api/poll' || pathname === '/api/status')) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(installerStatus));
    return;
  }

  // 9. Scan Status API
  if (req.method === 'GET' && pathname === '/api/scan/status') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ scanning: false, processed_files: examplePkgs.length, total_files: examplePkgs.length, progress: 100 }));
    return;
  }

  // 10. Settings API (with 2 SMB shares configured)
  if (req.method === 'GET' && pathname === '/api/settings') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(mockSettings));
    return;
  }

  if ((req.method === 'POST' || req.method === 'PUT') && pathname === '/api/settings') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        const newSettings = JSON.parse(body || '{}');
        Object.assign(mockSettings, newSettings);
      } catch (e) {}
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: true }));
    });
    return;
  }

  // 11. SMB Test Connection API
  if (req.method === 'POST' && pathname === '/api/smb/test') {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: true, message: 'Connected to Samba share successfully' }));
    });
    return;
  }

  // 12. Cache, Leftovers, Shortcut, Log APIs
  if (req.method === 'GET' && pathname === '/api/cache/stats') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ total_bytes: 14260000, total_count: 14, cache_path: '/data/pkgmgr/cache', drives: [] }));
    return;
  }

  if (req.method === 'POST' && pathname === '/api/cache/clear') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ success: true, freed_bytes: 14260000 }));
    return;
  }

  if (req.method === 'GET' && pathname === '/api/leftovers') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ leftovers: [], count: 0 }));
    return;
  }

  if (req.method === 'POST' && pathname === '/api/leftovers/delete') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ success: true, freed_bytes: 0, deleted_paths: [] }));
    return;
  }

  if (req.method === 'POST' && pathname === '/api/shortcut/install') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ success: true }));
    return;
  }

  if (req.method === 'GET' && pathname === '/api/icon-error') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ errors: [] }));
    return;
  }

  if (req.method === 'GET' && pathname === '/api/log') {
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('[PKG Manager Daemon Mock Log]\n[SCANNER] Scanned 14 packages across 4 sources\n[STREAM] Ready on port 8845\n');
    return;
  }

  // 12b. Direct-install live sessions (memory-backed mock of the RAM-only
  // live path: no files, Buffer holds bytes like the device ring).
  if (pathname.startsWith('/api/upload/')) {
    const readBody = () => new Promise((resolve) => {
      let b = '';
      req.on('data', (c) => { b += c; });
      req.on('end', () => resolve(b));
    });
    const liveUri = () => 'live:' + mockUpload.id;
    if (req.method === 'POST' && pathname === '/api/upload/icon') {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      const icon = Buffer.concat(chunks);
      const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
      const valid = mockUpload.active && req.headers['x-direct-owner'] === mockUpload.owner &&
        req.headers['x-direct-session'] === mockUpload.id &&
        icon.length >= 8 && icon.length <= 10 * 1024 * 1024 && icon.subarray(0, 8).equals(signature);
      if (valid) mockUpload.icon = icon;
      res.writeHead(valid ? 200 : 400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: valid }));
      return;
    }
    if (req.method === 'POST' && pathname === '/api/upload/check') {
      let details = {};
      try { details = JSON.parse(await readBody() || '{}'); } catch (e) {}
      if (!details.title_id || !['base', 'update', 'dlc'].includes(details.pkg_type) ||
        (details.pkg_type === 'dlc' && !details.content_id)) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Invalid package metadata' }));
        return;
      }
      const installed = examplePkgs.find((p) => p.title_id === details.title_id && p.is_installed);
      const dlc = examplePkgs.find((p) => p.content_id === details.content_id && p.is_dlc_installed);
      const compare = (a, b) => {
        const parts = (v) => String(v || '').replace(/^v/i, '').split('.').map(Number);
        const left = parts(a), right = parts(b);
        for (let i = 0; i < Math.max(left.length, right.length); i++) {
          if ((left[i] || 0) !== (right[i] || 0)) return (left[i] || 0) - (right[i] || 0);
        }
        return 0;
      };
      let reason = '';
      if (details.pkg_type !== 'base' && !installed) reason = 'Base package is not installed';
      else if (details.pkg_type === 'dlc' && dlc) reason = 'DLC is already installed';
      else if (installed && details.app_version && installed.installed_version &&
        compare(installed.installed_version, details.app_version) >= 0 && details.pkg_type !== 'dlc') {
        reason = 'Installed version is same or newer';
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ can_install: !reason, install_disabled_reason: reason,
        is_installed: !!installed, installed_version: installed?.installed_version || '' }));
      return;
    }
    if (req.method === 'POST' && pathname === '/api/upload/init') {
      const body = await readBody();
      let parsed = {};
      try { parsed = JSON.parse(body || '{}'); } catch (e) {}
      const total = Number(parsed.total) || 0;
      const filename = String(parsed.filename || '').split('/').pop();
      if (!filename || !(total > 0) || total > 2 * 1024 * 1024 * 1024) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, error: 'filename and total required' }));
        return;
      }
      if (mockUpload.active && (mockUpload.filename !== filename || mockUpload.total !== total)) {
        res.writeHead(409, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, error: 'Another upload is active' }));
        return;
      }
      if (!mockUpload.active) {
        mockUpload.active = true;
        mockUpload.id = crypto.randomBytes(8).toString('hex');
        mockUpload.owner = parsed.owner || '';
        mockUpload.filename = filename;
        mockUpload.total = total;
        mockUpload.received = 0;
        mockUpload.buf = Buffer.alloc(total);
        mockUpload.icon = null;
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: true, session_id: mockUpload.id, offset: mockUpload.received, ws_port: WS_MOCK_PORT }));
      return;
    }
    if (req.method === 'GET' && pathname === '/api/upload/status') {
      const headNeed = Math.min(1024 * 1024, mockUpload.total);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({
        active: mockUpload.active,
        session_id: mockUpload.id, filename: mockUpload.filename,
        total: mockUpload.total, received: mockUpload.received,
        served: mockUpload.received,
        complete: mockUpload.active && mockUpload.received === mockUpload.total,
        header_ready: mockUpload.active && mockUpload.received >= headNeed && headNeed > 0
      }));
      return;
    }
    if (req.method === 'POST' && pathname === '/api/upload/finish') {
      const nsegs = Math.ceil(mockUpload.total / (1024 * 1024));
      const allIn = mockUpload.present && mockUpload.present.size >= nsegs;
      if (!mockUpload.active || !allIn) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ success: false, error: 'Upload incomplete' }));
        return;
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: true, path: liveUri() }));
      return;
    }
    if (req.method === 'POST' && pathname === '/api/upload/cancel') {
      mockUpload.active = false;
      mockUpload.received = 0;
      mockUpload.buf = null;
      mockUpload.icon = null;
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ success: true }));
      return;
    }
  }

  if ((req.method === 'GET' || req.method === 'HEAD') && pathname === '/cache.appcache') {
    res.writeHead(200, { 'Content-Type': 'text/cache-manifest; charset=utf-8' });
    if (req.method === 'HEAD') res.end();
    else res.end('CACHE MANIFEST\n# Build: mock\n\nNETWORK:\n*\n');
    return;
  }

  if ((req.method === 'GET' || req.method === 'HEAD') && (pathname === '/version' || pathname === '/api/version')) {
    let ver = '1.3.0';
    try {
      const vh = fs.readFileSync(path.resolve(__dirname, '../include/version.h'), 'utf8');
      const m = vh.match(/#define\s+PKGMGR_VERSION\s+"([^"]+)"/);
      if (m) ver = m[1];
    } catch (e) {}
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    if (req.method === 'HEAD') res.end();
    else res.end(ver + '\n');
    return;
  }

  // 13. Static Files Serving (dist/ & public/)
  const cleanPath = pathname.replace(/^\/+/, '');
  const candidates = [
    path.resolve(__dirname, 'dist', cleanPath),
    path.resolve(__dirname, 'public', cleanPath)
  ];

  if (cleanPath === '' || cleanPath === 'index.html') {
    candidates.unshift(path.resolve(__dirname, 'dist/index.html'));
  }

  for (const filePath of candidates) {
    if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
      const ext = path.extname(filePath).toLowerCase();
      const mimeTypes = {
        '.html': 'text/html; charset=utf-8',
        '.svg': 'image/svg+xml',
        '.png': 'image/png',
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.css': 'text/css',
        '.js': 'application/javascript',
        '.ico': 'image/x-icon',
        '.json': 'application/json'
      };
      res.writeHead(200, { 'Content-Type': mimeTypes[ext] || 'application/octet-stream' });
      fs.createReadStream(filePath).pipe(res);
      return;
    }
  }

  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ error: 'Not found' }));
});

// Mock WS listener on :8846 (mirrors ws_upload.c framing: masked client
// frames, unmasked text acks, in-order binary chunks at mockUpload.received).
function wsSendText(sock, obj) {
  const payload = Buffer.from(JSON.stringify(obj));
  const n = payload.length;
  let hdr;
  if (n < 126) hdr = Buffer.from([0x81, n]);
  else if (n < 65536) { hdr = Buffer.alloc(4); hdr[0] = 0x81; hdr[1] = 126; hdr.writeUInt16BE(n, 2); }
  else { hdr = Buffer.alloc(10); hdr[0] = 0x81; hdr[1] = 127; hdr.writeBigUInt64BE(BigInt(n), 2); }
  sock.write(Buffer.concat([hdr, payload]));
}

server.on('upgrade', mockWsUpgrade);

const wsMock = http.createServer();
wsMock.on('upgrade', mockWsUpgrade);
wsMock.listen(WS_MOCK_PORT, '0.0.0.0');

function mockWsUpgrade(req, sock) {
  const url = new URL(req.url, 'http://127.0.0.1:8846');
  const key = req.headers['sec-websocket-key'];
  const okWs = (req.headers.upgrade || '').toLowerCase() === 'websocket';
  if (url.pathname !== '/ws/upload' || !okWs || !key) {
    sock.write('HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n');
    sock.destroy();
    return;
  }
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');

  let buf = Buffer.alloc(0);
  let textFrag = '';
  let textOn = false;
  let fragOp = 0;
  let pendingSeg = -1;
  let fragBin = [];
  sock.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) return;
      const b0 = buf[0], b1 = buf[1];
      const opcode = b0 & 0x0f, fin = !!(b0 & 0x80), masked = !!(b1 & 0x80);
      let plen = b1 & 0x7f, hlen = 2;
      if (plen === 126) {
        if (buf.length < 4) return;
        plen = buf.readUInt16BE(2); hlen = 4;
      } else if (plen === 127) {
        if (buf.length < 10) return;
        plen = Number(buf.readBigUInt64BE(2)); hlen = 10;
      }
      if (!masked || buf.length < hlen + 4 + plen) return;
      const mask = buf.slice(hlen, hlen + 4);
      const pay = buf.slice(hlen + 4, hlen + 4 + plen);
      for (let i = 0; i < pay.length; i++) pay[i] ^= mask[i % 4];
      buf = buf.slice(hlen + 4 + plen);
      if (opcode === 0x8) { sock.end(); return; }
      if (opcode === 0x9) { sock.write(Buffer.from([0x8a, 0x00])); continue; }
      if (opcode === 0xa) continue;
      if (opcode === 0x1 || (opcode === 0x0 && fragOp === 1)) {
        if (opcode === 0x1) { textFrag = ''; textOn = true; fragOp = 1; }
        if (!textOn) { sock.end(); return; }
        textFrag += pay.toString('utf8');
        if (!fin) continue;
        textOn = false;
        let msg = {};
        try { msg = JSON.parse(textFrag); } catch (e) { wsSendText(sock, { op: 'error', error: 'bad json' }); continue; }
        if (msg.op === 'init') {
          const total = Number(msg.total) || 0;
          const filename = String(msg.filename || '').split('/').pop();
          if (!filename || !(total > 0) || total > 2 * 1024 * 1024 * 1024) { wsSendText(sock, { op: 'error', error: 'init needs filename+total' }); continue; }
          if (!mockUpload.active) {
            mockUpload.active = true;
            mockUpload.id = crypto.randomBytes(8).toString('hex');
            mockUpload.filename = filename;
            mockUpload.total = total;
            mockUpload.received = 0;
            mockUpload.buf = Buffer.alloc(total);
          }
          wsSendText(sock, { op: 'ready', session_id: mockUpload.id, offset: mockUpload.received });
        } else if (msg.op === 'status') {
          wsSendText(sock, { active: mockUpload.active, total: mockUpload.total, received: mockUpload.received });
        } else if (msg.op === 'finish') {
          if (mockUpload.active && mockUpload.received === mockUpload.total) {
            wsSendText(sock, { op: 'complete', path: 'live:' + mockUpload.id });
          } else {
            wsSendText(sock, { op: 'error', error: 'incomplete', received: mockUpload.received, total: mockUpload.total });
          }
        } else if (msg.op === 'cancel') {
          mockUpload.active = false;
          mockUpload.received = 0;
          mockUpload.buf = null;
          mockUpload.icon = null;
          mockUpload.present = null;
          wsSendText(sock, { op: 'cancelled' });
        } else if (msg.op === 'seg') {
          const S = Number(msg.seg);
          pendingSeg = Number.isInteger(S) && S >= 0 ? S : -1;
        } else {
          wsSendText(sock, { op: 'error', error: 'unknown op' });
        }
      } else if (opcode === 0x2 || (opcode === 0x0 && fragOp === 2)) {
        if (opcode === 0x2) { fragOp = 2; pendingSeg = -1; fragBin = []; }
        if (!mockUpload.active || !mockUpload.buf) { wsSendText(sock, { op: 'error', error: 'no session' }); continue; }
        fragBin.push(pay);
        if (!fin) continue;
        // Whole message reassembled: apply to the named segment.
        const msg = Buffer.concat(fragBin);
        fragBin = [];
        const SEG = 1024 * 1024;
        const total = mockUpload.total;
        const nsegs = Math.ceil(total / SEG);
        const S = pendingSeg;
        const expLen = S < nsegs - 1 ? SEG : total - S * SEG;
        if (S < 0 || S >= nsegs || msg.length !== expLen) {
          wsSendText(sock, { op: 'error', error: 'bad segment' });
          continue;
        }
        msg.copy(mockUpload.buf, S * SEG);
        if (!mockUpload.present) mockUpload.present = new Set();
        mockUpload.present.add(S);
        // Contiguous run from 0 (resume offset semantics).
        let contig = 0;
        while (mockUpload.present.has(contig)) contig++;
        const lastLen = total - (nsegs - 1) * SEG;
        mockUpload.received = 0;
        for (let s = 0; s < contig; s++) mockUpload.received += (s < nsegs - 1 ? SEG : lastLen);
        wsSendText(sock, { op: 'ack', seg: S });
      }
    }
  });
}

server.listen(PORT, '0.0.0.0', () => {
  console.log(`=======================================================`);
  console.log(` PKG Manager Mock Server running!`);
  console.log(` Dashboard URL : http://localhost:${PORT}`);
  console.log(` LAN Access    : http://0.0.0.0:${PORT}`);
  console.log(` Sources       : USB Drive 0, Blu-ray Disc, 2 Samba shares`);
  console.log(` Packages      : 14 packages (Fictional titles, clean art)`);
  console.log(`=======================================================`);
});
