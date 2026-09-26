import SmbFileBrowser from './components/views/SmbFileBrowser';
import React, { useState, useEffect, useMemo, useRef, useCallback } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import BlurIcon, { iconUrlFor } from './BlurIcon';
import { formatBytes, formatEta, formatVersion } from './utils/formatters';
import { getSourceInfo } from './utils/sourceInfo';
import { getBrowserTitle, getFullVersion, getLocalizedTitle } from './utils/title';
import { DONATE_URL, isPlayStation, DONATE_MODAL_STORAGE_KEY, DONATE_MODAL_INTERVAL_MS, ALL_SOURCES_DRIVE } from './constants/config';
import { checkVersion } from './api/health';
import { getStorage } from './api/storage';
import { getDrives } from './api/drives';
import { getPackages, refreshPackages, getScanStatus, waitForScan, quickScan, shouldAutoScanDrive } from './api/packages';
import { pollStatus, installPackage, cancelInstall } from './api/installer';
import { getSettings, saveSettings, closeManager } from './api/settings';
import { installShortcut as apiInstallShortcut } from './api/settings';
import { getCacheStats, clearCache } from './api/cache';
import { scanLeftovers as apiScanLeftovers, deleteLeftover } from './api/leftovers';
import { testSmb } from './api/smb';

import { useToast } from './hooks/useToast';
import { useCache } from './hooks/useCache';
import { useLeftovers } from './hooks/useLeftovers';
import { useSettings } from './hooks/useSettings';
import { useSmb } from './hooks/useSmb';
import { useInstaller } from './hooks/useInstaller';
import { useDonation } from './hooks/useDonation';
import { useHistoryNavigation, getRouteFromHash, resolveDrive } from './hooks/useHistoryNavigation';
import { useModalInert } from './hooks/useModalInert';
import { useDirectUpload } from './hooks/useDirectUpload';
import { uploadStatus } from './api/directInstall';

import OfflineScreen from './components/screens/OfflineScreen';
import LoadingScreen from './components/screens/LoadingScreen';
import WaitingForPartScreen from './components/screens/WaitingForPartScreen';
import InstallingScreen from './components/screens/InstallingScreen';
import ScanningScreen from './components/screens/ScanningScreen';

import Toast from './components/layout/Toast';
import Header from './components/layout/Header';
import Footer from './components/layout/Footer';

import SmbManagementView from './components/views/SmbManagementView';
import SettingsView from './components/views/SettingsView';
import TitleDetailView from './components/views/TitleDetailView';
import PackageGridView from './components/views/PackageGridView';
import DrivesView from './components/views/DrivesView';

import DirectInstallView from './components/views/DirectInstallView';
import DonateModal from './components/modals/DonateModal';
import ClearCacheModal from './components/modals/ClearCacheModal';
import SmbShareModal from './components/modals/SmbShareModal';
import DeleteLeftoverModal from './components/modals/DeleteLeftoverModal';

const CACHE_SCHEMA_STORAGE_KEY = 'pkgmgr_cache_schema_version';
// Increment only when persisted package metadata or icon cache must be rebuilt.
// Ordinary ELF releases must leave this unchanged to avoid needless full scans.
const CACHE_SCHEMA_VERSION = '1';

export default function App() {
  const [isOffline, setIsOffline] = useState(false);
  const [isClosing, setIsClosing] = useState(false);
  const [appVersion, setAppVersion] = useState('');
  
  const [drives, setDrives] = useState([]);
  const [selectedDrive, setSelectedDrive] = useState(() => {
    if (typeof window !== 'undefined' && window.location?.hash) {
      const route = getRouteFromHash(window.location.hash);
      if (route.type === 'drive' || route.type === 'title') {
        const driveId = route.driveId || '__all__';
        return resolveDrive(driveId, []);
      }
    }
    try {
      const saved = localStorage.getItem('pkgmgr_settings');
      if (saved) {
        const parsed = JSON.parse(saved);
        if (parsed.all_sources_mode) {
          return ALL_SOURCES_DRIVE;
        }
      }
    } catch (e) {}
    return null;
  });
  const [packages, setPackages] = useState([]);
  const [storage, setStorage] = useState(null);
  const [loadingDrives, setLoadingDrives] = useState(true);
  const [loadingPackages, setLoadingPackages] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [sortBy, setSortBy] = useState('date-desc');
  const [selectedTitleId, setSelectedTitleId] = useState(null);
  const [showDirectInstall, setShowDirectInstall] = useState(false);
  const directTabId = useRef(Math.random().toString(36).slice(2) + Date.now());
  const directUpload = useDirectUpload(directTabId.current);
  const directTransferActive = directUpload.state === 'uploading' || directUpload.installing;
  const directLeaseHeld = Boolean(directUpload.sessionId) &&
    directUpload.state !== 'idle' && directUpload.state !== 'canceled';

  useEffect(() => {
    if (!directLeaseHeld) return;
    const mark = () => localStorage.setItem('directInstallLease', JSON.stringify({
      tab: directTabId.current, time: Date.now(), session: directUpload.sessionId
    }));
    mark();
    const timer = setInterval(mark, 2000);
    return () => {
      clearInterval(timer);
      try {
        const lease = JSON.parse(localStorage.getItem('directInstallLease') || '{}');
        if (lease.tab === directTabId.current) localStorage.removeItem('directInstallLease');
      } catch (e) {}
    };
  }, [directLeaseHeld, directUpload.sessionId]);

  useEffect(() => {
    if (!directTransferActive) return;
    const warn = (event) => {
      event.preventDefault();
      event.returnValue = '';
    };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [directTransferActive]);

  const selectedDriveRef = useRef(selectedDrive);
  selectedDriveRef.current = selectedDrive;
  useEffect(() => {
    selectedDriveRef.current = selectedDrive;
  }, [selectedDrive]);
  
  const selectedTitleIdRef = useRef(selectedTitleId);
  selectedTitleIdRef.current = selectedTitleId;
  useEffect(() => {
    selectedTitleIdRef.current = selectedTitleId;
  }, [selectedTitleId]);

  const [packagePage, setPackagePage] = useState(0);
  useEffect(() => { setPackagePage(0); }, [searchQuery, sortBy, selectedDrive?.id]);

  const [scanStatus, setScanStatus] = useState({
    is_scanning: false,
    total_files: 0,
    processed_files: 0,
    current_drive: '',
    current_file: '',
    progress: 0
  });

  const scrollPositionRef = useRef(0);
  const shouldRestoreScrollRef = useRef(false);
  const detailScrollPositionRef = useRef(0);
  const shouldRestoreDetailScrollRef = useRef(false);
  const checkOnlineRef = useRef(null);

  const refreshingRef = useRef(refreshing);
  useEffect(() => {
    refreshingRef.current = refreshing;
  }, [refreshing]);

  const scanStatusRef = useRef(scanStatus);
  useEffect(() => {
    scanStatusRef.current = scanStatus;
  }, [scanStatus]);

  const quickScanInProgressRef = useRef(false);

  // === HOOKS ===
  const { notification, showToast } = useToast();

  const fetchStorage = async () => {
    try {
      const data = await getStorage();
      setStorage(data);
    } catch (err) {}
  };

  const fetchDrives = async () => {
    try {
      const data = await getDrives();
      setDrives(data);
    } catch (err) {} finally {
      setLoadingDrives(false);
    }
  };

  const fetchPackagesForDrive = async (drive, silent = false) => {
    const targetDrive = drive || selectedDriveRef.current;
    if (!targetDrive) return;
    if (!silent) setLoadingPackages(true);
    try {
      const driveKey = targetDrive.id || targetDrive.path;
      const data = await getPackages(driveKey);
      const processed = Array.isArray(data)
        ? data.map((pkg) => {
            const locTitle = getLocalizedTitle(pkg);
            return locTitle ? { ...pkg, title_name: locTitle } : pkg;
          })
        : data;
      setPackages(processed);
    } catch (err) {
      // ignore
    } finally {
      if (!silent) setLoadingPackages(false);
    }
  };

  const refreshAll = async (resume = false) => {
    if (refreshingRef.current) return false;
    refreshingRef.current = true;
    setRefreshing(true);
    setScanStatus({
      is_scanning: true,
      total_files: 0,
      processed_files: 0,
      current_drive: 'Scanning storage media...',
      current_file: '',
      progress: 0
    });

    try {
      if (resume === true) await waitForScan(setScanStatus);
      else await refreshPackages(setScanStatus);
      await Promise.all([fetchDrives(), fetchStorage()]);
      if (selectedDriveRef.current) {
        await fetchPackagesForDrive(selectedDriveRef.current, true);
      }
      showToast('Refreshed package catalog', 'success');
      return true;
    } catch (err) {
      showToast('Error refreshing: ' + err.message, 'error');
      return false;
    } finally {
      refreshingRef.current = false;
      setScanStatus((prev) => ({ ...prev, is_scanning: false }));
      setTimeout(() => {
        setRefreshing(false);
      }, 400);
    }
  };

  const handleQuickRescan = async (forced = false) => {
    if (refreshingRef.current) return false;
    if (quickScanInProgressRef.current) {
      let waitCount = 0;
      while (quickScanInProgressRef.current && waitCount++ < 30) {
        await new Promise((r) => setTimeout(r, 100));
      }
    }
    refreshingRef.current = true;
    setRefreshing(true);

    try {
      const targetDrive = selectedDriveRef.current;
      const targetId = targetDrive && targetDrive.id && targetDrive.id !== '__all__' ? targetDrive.id : null;
      const data = await quickScan(targetId);
      await Promise.all([
        fetchDrives(),
        fetchStorage(),
        fetchPackagesForDrive(selectedDriveRef.current, true)
      ]);
      if (data && data.changed) {
        showToast('Catalog updated', 'success');
      } else if (!forced) {
        showToast('No changes found', 'info');
      }
      return true;
    } catch (err) {
      showToast('Error during rescan: ' + err.message, 'error');
      return false;
    } finally {
      refreshingRef.current = false;
      setTimeout(() => {
        setRefreshing(false);
      }, 400);
    }
  };

  const {
    cacheStats, loadingStats, showClearCacheModal, setShowClearCacheModal,
    clearingCache, fetchCacheStats, handleClearCache
  } = useCache({ showToast, onRescan: refreshAll });

  const {
    settings, setSettings, showSettings, setShowSettings, showSmbPage, setShowSmbPage,
    installingShortcut, fetchSettings, handleSaveSettings, handleInstallShortcut
  } = useSettings({
    showToast, selectedDriveRef, setSelectedDrive, fetchPackagesForDrive, fetchDrives, fetchStorage, setPackages
  });

  const {
    leftoversData, scanningLeftovers, selectedLeftoverToDelete, setSelectedLeftoverToDelete,
    deletingLeftover, handleScanLeftovers, handleConfirmDeleteLeftover, handleOpenLeftoverCleanupForTitle
  } = useLeftovers({ showToast, fetchStorage, fetchPackagesForDrive, selectedDriveRef });

  const {
    showSmbModal, setShowSmbModal, smbEditIndex, setSmbEditIndex, smbForm, setSmbForm,
    smbTesting, smbTestResult, setSmbTestResult, handleSaveSmbShare, handleRemoveSmbShare, handleToggleSmbShare, handleTestSmbConnection
  } = useSmb({ settings, showToast, handleSaveSettings, refreshAll, fetchDrives });

  const groupedTitles = useMemo(() => {
    const isAllSources = selectedDrive && selectedDrive.id === '__all__';
    const filtered = packages.filter((p) => {
      if (p.filename && p.filename.startsWith('.')) return false;
      if (!searchQuery.trim()) return true;
      const q = searchQuery.toLowerCase();
      let matchLocalized = false;
      if (p.localized_titles) {
        let lt = p.localized_titles;
        if (typeof lt === 'string' && lt.trim().startsWith('{')) {
          try { lt = JSON.parse(lt); } catch (e) { lt = null; }
        }
        if (lt && typeof lt === 'object') {
          matchLocalized = Object.values(lt).some((v) => typeof v === 'string' && v.toLowerCase().includes(q));
        }
      }
      return (
        matchLocalized ||
        (p.title_name && p.title_name.toLowerCase().indexOf(q) !== -1) ||
        (p.title_id && p.title_id.toLowerCase().indexOf(q) !== -1) ||
        (p.content_id && p.content_id.toLowerCase().indexOf(q) !== -1) ||
        (p.app_version && p.app_version.toLowerCase().indexOf(q) !== -1) ||
        (p.pkg_type && p.pkg_type.toLowerCase().indexOf(q) !== -1)
      );
    });

    const groupMap = new Map();
    for (const pkg of filtered) {
      const baseKey = (pkg.title_id && pkg.title_id.trim() && pkg.title_id.trim().toUpperCase() !== 'UNKNOWN')
        ? pkg.title_id.trim().toUpperCase()
        : (pkg.title_name && pkg.title_name !== 'Unknown Package' && pkg.title_name !== 'Package' ? pkg.title_name : pkg.filename || pkg.path || 'unknown');

      const srcInfo = getSourceInfo(pkg.path, drives);
      const key = isAllSources ? `${baseKey}__${srcInfo.id}` : baseKey;

      if (!groupMap.has(key)) {
        groupMap.set(key, { items: [], srcInfo });
      }
      groupMap.get(key).items.push(pkg);
    }

    const groups = [];
    for (const [key, { items, srcInfo }] of groupMap.entries()) {
      // Find base package
      const base = items.find((p) => p.pkg_type === 'base') || null;

      // Find update packages, sorted newest version first
      const updates = items.filter((p) => p.pkg_type === 'update');
      updates.sort((a, b) => {
        const verA = (a.app_version || '').replace(/^v/, '');
        const verB = (b.app_version || '').replace(/^v/, '');
        return verB.localeCompare(verA, undefined, { numeric: true, sensitivity: 'base' });
      });

      // Find DLC packages
      const dlcs = items.filter((p) => p.pkg_type === 'dlc');

      // Find other/unknown packages
      const others = items.filter(
        (p) => p.pkg_type !== 'base' && p.pkg_type !== 'update' && p.pkg_type !== 'dlc'
      );

      // Representative package
      const primaryPkg = base || updates[0] || dlcs[0] || others[0] || items[0];

      // Image package (primaryPkg if it has icon, or first item that has icon)
      const imagePkg = (primaryPkg && primaryPkg.has_icon)
        ? primaryPkg
        : items.find((p) => p.has_icon) || primaryPkg;

      const nonGenericTitle = [base, updates[0], dlcs[0]].concat(items)
        .map(function(p) { return p ? p.title_name : undefined; })
        .find(function(t) { return t && t !== 'Package' && t !== 'Unknown Package'; });
      const titleName = nonGenericTitle || primaryPkg.title_name || primaryPkg.filename || 'Unknown Package';
      const titleId = primaryPkg.title_id || (base && base.title_id) || '';

      // Latest detected update version formatted cleanly
      var latestUpdateVer = null;
      if (updates.length > 0 && updates[0].app_version) {
        latestUpdateVer = formatVersion(updates[0].app_version);
      }

      // Multi-part package tracking and sizes
      const isBaseMultipart = base ? !!base.is_multipart && (Number(base.total_parts) > 1) : false;
      const partIndex = (base && base.part_index) ? base.part_index : 1;
      const totalParts = (base && base.total_parts) ? base.total_parts : 1;
      const firstPartSize = base ? (Number(base.file_size) || 0) : 0;
      const baseFullSize = base ? (Number(base.total_pkg_size || base.file_size) || 0) : 0;

      // Check if ANY package in this group is multi-part
      const hasMultipart = items.some((p) => p.is_multipart && (Number(p.total_parts) > 1));
      const totalDriveSize = items.reduce((sum, p) => sum + (Number(p.file_size) || 0), 0);
      const totalFullSize = items.reduce((sum, p) => sum + (Number(p.total_pkg_size || p.file_size) || 0), 0);

      // Latest mtime across all items in group
      const latestMtime = Math.max.apply(null, items.map((p) => Number(p.mtime) || 0));
      const totalSize = totalFullSize;

      const isBaseInstalled = base ? base.is_installed : items.some((p) => p.is_installed);
      const rawInstalledVer = base
        ? base.installed_version
        : ((items.find((p) => p.installed_version) || {}).installed_version || '');
      const installedVersion = formatVersion(rawInstalledVer);

      const hasLeftover = base ? !!base.has_leftover : items.some((p) => p.has_leftover);
      const leftoverDesc = (base && base.leftover_desc) || ((items.find((p) => p.leftover_desc) || {}).leftover_desc || '');

      const isPartiallyInstalled = base ? !!base.is_partially_installed : items.some((p) => p.is_partially_installed);
      const partialDesc = (base && base.partial_desc) || ((items.find((p) => p.partial_desc) || {}).partial_desc || '');

      const hasBaseOnDrive = !!base;
      const hasNewBase = hasBaseOnDrive && (!isBaseInstalled || (base && base.can_install !== false));

      const isLatestUpdateInstalled = updates.length > 0 && isBaseInstalled && (
        updates[0].can_install === false &&
        updates[0].install_disabled_reason === 'Installed version is same or newer'
      );
      const hasNewUpdate = isBaseInstalled && updates.length > 0 && !isLatestUpdateInstalled;

      const uninstalledDlcs = dlcs.filter((d) => !d.is_dlc_installed && d.install_disabled_reason !== 'DLC is already installed');
      const areAllDlcsInstalled = dlcs.length > 0 && isBaseInstalled && uninstalledDlcs.length === 0;
      const hasNewDlc = isBaseInstalled && dlcs.length > 0 && uninstalledDlcs.length > 0;

      const isBaseUpToDate = isBaseInstalled && (!base || base.can_install === false);
      const isEverythingInstalled = isBaseUpToDate &&
        (updates.length === 0 || isLatestUpdateInstalled) &&
        (dlcs.length === 0 || areAllDlcsInstalled);

      groups.push({
        id: key,
        title_id: titleId,
        title_name: titleName,
        sourceName: srcInfo.name,
        sourceType: srcInfo.type,
        sourceId: srcInfo.id,
        primaryPkg,
        imagePkg,
        has_icon: !!(imagePkg && imagePkg.has_icon),
        iconPath: imagePkg ? imagePkg.path : '',
        base,
        updates,
        dlcs,
        others,
        items,
        latestUpdateVersion: latestUpdateVer,
        dlcCount: dlcs.length,
        latestMtime,
        totalSize,
        totalDriveSize,
        totalFullSize,
        hasMultipart,
        isMultipart: isBaseMultipart,
        isBaseMultipart,
        partIndex,
        totalParts,
        firstPartSize,
        baseFullSize,
        isBaseInstalled,
        installedVersion,
        hasLeftover,
        leftoverDesc,
        isPartiallyInstalled,
        partialDesc,
        hasBaseOnDrive,
        hasNewBase,
        isLatestUpdateInstalled,
        hasNewUpdate,
        uninstalledDlcs,
        areAllDlcsInstalled,
        hasNewDlc,
        isEverythingInstalled
      });
    }

    return groups.sort((a, b) => {
      if (settings.move_installed_to_end) {
        if (a.isEverythingInstalled !== b.isEverythingInstalled) {
          return a.isEverythingInstalled ? 1 : -1;
        }
      }

      const nameA = a.title_name.toLowerCase();
      const nameB = b.title_name.toLowerCase();

      if (sortBy === 'name-asc') {
        return nameA.localeCompare(nameB);
      }
      if (sortBy === 'name-desc') {
        return nameB.localeCompare(nameA);
      }
      if (sortBy === 'date-desc') {
        if (b.latestMtime !== a.latestMtime) return b.latestMtime - a.latestMtime;
        return nameA.localeCompare(nameB);
      }
      if (sortBy === 'date-asc') {
        if (a.latestMtime !== b.latestMtime) return a.latestMtime - b.latestMtime;
        return nameA.localeCompare(nameB);
      }
      if (sortBy === 'size-desc') {
        if (b.totalSize !== a.totalSize) return b.totalSize - a.totalSize;
        return nameA.localeCompare(nameB);
      }
      if (sortBy === 'size-asc') {
        if (a.totalSize !== b.totalSize) return a.totalSize - b.totalSize;
        return nameA.localeCompare(nameB);
      }
      return 0;
    });
  }, [packages, searchQuery, sortBy, settings.move_installed_to_end, selectedDrive, drives]);

  const selectedTitle = useMemo(() => {
    if (!selectedTitleId) return null;
    return groupedTitles.find((g) => g.id === selectedTitleId) || null;
  }, [groupedTitles, selectedTitleId]);


  const {
    installerStatus, setInstallerStatus, batchInstall, setBatchInstall, initialStatusLoaded, setInitialStatusLoaded,
    etaInfo, isWaitingForPart, isBatchActive, isInstalling, isDiscSource, speedCalcRef, wasInstallingRef, batchInstallRef,
    installerStatusRef, fetchStatus, handleInstall, handleInstallBaseAndUpdate, handleCancel
  } = useInstaller({
    showToast,
    fetchStorage,
    fetchPackagesForDrive,
    selectedDriveRef,
    selectedTitleIdRef,
    detailScrollPositionRef,
    shouldRestoreDetailScrollRef,
    storage,
    selectedTitle,
  });

  const {
    showDonateQr, setShowDonateQr, showDonateModal, setShowDonateModal, donateNeverNotice, setDonateNeverNotice,
    showModalQr, setShowModalQr, handleCloseDonateModal, handleNeverShowDonateModal
  } = useDonation({ initialStatusLoaded, isInstalling, isWaitingForPart });

  const triggerQuickScan = useCallback(async (drive) => {
    // 1. Never interrupt an active installation or optical disc wait
    if ((installerStatusRef.current && installerStatusRef.current.is_installing) || (installerStatusRef.current && installerStatusRef.current.waiting_for_disc)) {
      return;
    }
    // 2. Never collide with a user-initiated full rescan
    if (refreshingRef.current || (scanStatusRef.current && scanStatusRef.current.is_scanning)) {
      return;
    }
    // 3. Do not overlap quick scans
    if (quickScanInProgressRef.current) {
      return;
    }

    quickScanInProgressRef.current = true;
    try {
      const targetDrive = drive || selectedDriveRef.current;
      const targetId = targetDrive && targetDrive.id && targetDrive.id !== '__all__' ? targetDrive.id : null;
      const data = await quickScan(targetId);
      if (data && data.changed) {
        // Silently update catalog, drives, and storage without blocking UI or showing overlay
        await Promise.all([
          fetchDrives(),
          fetchStorage(),
          fetchPackagesForDrive(selectedDriveRef.current, true)
        ]);
      }
    } catch (err) {
      // Background quick scan failures are silent
    } finally {
      quickScanInProgressRef.current = false;
    }
  }, []);

  // History and Back navigation handlers (supports controller Circle button)
  const {
    handleSelectDrive,
    handleBackToDrives,
    handleOpenTitle,
    handleBackToPackages,
    handleOpenSettings,
    handleCloseSettings,
    handleOpenSmb,
    handleCloseSmb,
    handleOpenDirectInstall,
    handleCloseDirectInstall,
  } = useHistoryNavigation({
    setSelectedDrive,
    selectedDriveRef,
    setSelectedTitleId,
    selectedTitleIdRef,
    setShowSettings,
    setShowSmbPage,
    setShowDirectInstall,
    directTransferActive,
    drives,
    fetchPackagesForDrive,
    fetchDrives,
    fetchStorage,
    fetchCacheStats,
    scrollPositionRef,
    detailScrollPositionRef,
    shouldRestoreScrollRef,
    shouldRestoreDetailScrollRef,
    setPackages,
    setSearchQuery,
    triggerQuickScan,
    installerStatus,
    isBatchActive,
    showDonateModal,
    handleCloseDonateModal,
    showClearCacheModal,
    setShowClearCacheModal,
    showSmbModal,
    setShowSmbModal,
    selectedLeftoverToDelete,
    setSelectedLeftoverToDelete,
    showToast,
    initialRoute: selectedDrive ? { type: 'drive', driveId: selectedDrive.id || '__all__' } : { type: 'drives' },
  });
  const handleCloseDirectInstallRef = useRef(handleCloseDirectInstall);
  handleCloseDirectInstallRef.current = handleCloseDirectInstall;

  const openDirectInstall = useCallback(async () => {
    if (isPlayStation) return false;
    try {
      const status = await uploadStatus();
      if (status.active) {
        let lease = {};
        try { lease = JSON.parse(localStorage.getItem('directInstallLease') || '{}'); } catch (e) {}
        const anotherWindow = lease.tab && lease.tab !== directTabId.current && Date.now() - lease.time < 10000;
        const anotherSession = status.session_id !== sessionStorage.getItem('directInstallSession');
        if (anotherWindow || anotherSession) {
          window.alert('Direct Install is already active in another window. Finish it there first.');
          return false;
        }
      }
    } catch (e) {
      showToast('Could not check Direct Install status', 'error');
      return false;
    }
    handleOpenDirectInstall();
    return true;
  }, [handleOpenDirectInstall, isPlayStation, showToast]);

  useEffect(() => {
    if (isPlayStation) return;

    const hasFiles = (event) => Array.from(event.dataTransfer?.types || []).includes('Files');
    const onDragOver = (event) => {
      if (!hasFiles(event)) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    };
    const onDrop = async (event) => {
      if (event.__pkgManagerDropHandled || !hasFiles(event) || !event.dataTransfer?.files?.length) return;
      event.preventDefault();
      event.__pkgManagerDropHandled = true;

      // Keep the current page and upload session intact while an install is active.
      if (directUpload.state === 'uploading' || directUpload.state === 'checking' || directUpload.installing) return;

      const file = event.dataTransfer.files[0];
      if (await openDirectInstall()) directUpload.selectFile(file);
    };

    window.addEventListener('dragover', onDragOver);
    window.addEventListener('drop', onDrop);
    return () => {
      window.removeEventListener('dragover', onDragOver);
      window.removeEventListener('drop', onDrop);
    };
  }, [directUpload.installing, directUpload.selectFile, directUpload.state, isPlayStation, openDirectInstall]);

  useEffect(() => {
    if (!showDirectInstall || directUpload.sessionId) return;
    let stopped = false;
    let checking = false;
    const check = async () => {
      if (checking) return;
      checking = true;
      try {
        const status = await uploadStatus();
        if (stopped || !status.active) return;
        const lease = JSON.parse(localStorage.getItem('directInstallLease') || '{}');
        const anotherWindow = lease.tab && lease.tab !== directTabId.current && Date.now() - lease.time < 10000;
        const anotherSession = status.session_id !== sessionStorage.getItem('directInstallSession');
        if (anotherWindow || anotherSession) {
          stopped = true;
          window.alert('Direct Install is active in another window.');
          handleCloseDirectInstallRef.current();
        }
      } catch (e) {} finally {
        checking = false;
      }
    };
    check();
    const timer = setInterval(check, 3000);
    return () => { stopped = true; clearInterval(timer); };
  }, [directUpload.sessionId, showDirectInstall]);

  const isAnyModalOpen = Boolean(
    showDonateModal ||
    showClearCacheModal ||
    showSmbModal ||
    selectedLeftoverToDelete
  );
  useModalInert(isAnyModalOpen);

  useEffect(() => {
    document.title = getBrowserTitle();
    let unmounted = false;
    let retryTimer = null;

    const checkOnline = async () => {
      let offline = false;
      let cacheSchemaRescanCompleted = false;
      try {
        const v = await checkVersion();
        if (!unmounted) {
          setAppVersion(v);
          setIsOffline(false);
          document.title = getBrowserTitle(v);
        }

        let previousCacheSchema = null;
        try {
          previousCacheSchema = localStorage.getItem(CACHE_SCHEMA_STORAGE_KEY);
        } catch (e) {}

        // A missing marker is the first launch for this browser (including
        // upgrades from the old app-version marker); preserve its existing cache.
        const cacheSchemaChanged = Boolean(
          previousCacheSchema && previousCacheSchema !== CACHE_SCHEMA_VERSION
        );
        if (cacheSchemaChanged && !unmounted) {
          try {
            const data = await clearCache();
            if (!data || data.success !== true) {
              throw new Error((data && data.error) || 'Cache clear failed');
            }
            // The cache is gone, so record the schema even if the scan request
            // later disconnects; startup will rebuild the missing manifest.
            try { localStorage.setItem(CACHE_SCHEMA_STORAGE_KEY, CACHE_SCHEMA_VERSION); } catch (e) {}
            cacheSchemaRescanCompleted = await refreshAll();
          } catch (err) {
            showToast('Failed to refresh cache after cache format change: ' + err.message, 'error');
          }
        }

        // If invalidation failed, retain the previous marker and retry next startup.
        if (!cacheSchemaChanged || cacheSchemaRescanCompleted) {
          try { localStorage.setItem(CACHE_SCHEMA_STORAGE_KEY, CACHE_SCHEMA_VERSION); } catch (e) {}
        }
      } catch (err) {
        offline = true;
      }

      if (offline) {
        if (!unmounted) {
          setIsOffline(true);
        }
        retryTimer = setTimeout(checkOnline, 4000);
        return;
      }

      // Reattach after a browser reload without starting another scan.
      if (!cacheSchemaRescanCompleted) {
        try {
          const status = await getScanStatus();
          if (status.is_scanning) cacheSchemaRescanCompleted = await refreshAll(true);
        } catch (e) {}
      }
      fetchDrives();
      fetchStorage();
      fetchStatus();
      fetchSettings();

      if (!cacheSchemaRescanCompleted) {
        if (selectedDriveRef.current) {
          fetchPackagesForDrive(selectedDriveRef.current);
        } else if (settings.all_sources_mode) {
          setSelectedDrive(ALL_SOURCES_DRIVE);
          fetchPackagesForDrive(ALL_SOURCES_DRIVE);
        }

        triggerQuickScan(selectedDriveRef.current);
      }
    };

    checkOnlineRef.current = checkOnline;
    checkOnline();

    return () => {
      unmounted = true;
      if (retryTimer) clearTimeout(retryTimer);
    };
  }, []);

  useEffect(() => {
    if (isOffline) return;
    // Poll status faster (1s) when active install or waiting for disc, standard (3s) when idle
    const intervalTime = (installerStatus.is_installing || installerStatus.waiting_for_disc) ? 1000 : 3000;
    const interval = setInterval(() => {
      fetchStatus();
    }, intervalTime);
    return () => clearInterval(interval);
  }, [installerStatus.is_installing, installerStatus.waiting_for_disc, isOffline]);

  useEffect(() => {
    if (isOffline) return;
    // Poll individual local drives; network discovery runs on explicit navigation/rescan.
    const interval = setInterval(() => {
      const curDrive = selectedDriveRef.current;
      if (shouldAutoScanDrive(curDrive)) {
        triggerQuickScan(curDrive);
      }
    }, 15000);
    return () => clearInterval(interval);
  }, [triggerQuickScan, isOffline]);

  useEffect(() => {
    if (!selectedTitleId && !isInstalling && !isWaitingForPart && !batchInstall && shouldRestoreScrollRef.current) {
      shouldRestoreScrollRef.current = false;
      const targetY = scrollPositionRef.current || 0;
      const restore = () => {
        window.scrollTo(0, targetY);
        if (document.documentElement) document.documentElement.scrollTop = targetY;
        if (document.body) document.body.scrollTop = targetY;
      };
      restore();
      requestAnimationFrame(() => {
        restore();
        setTimeout(restore, 20);
        setTimeout(restore, 80);
      });
    }
  }, [selectedTitleId, isInstalling, isWaitingForPart, batchInstall]);

  useEffect(() => {
    if (!isInstalling && !isWaitingForPart && !batchInstall && selectedTitleId && shouldRestoreDetailScrollRef.current) {
      shouldRestoreDetailScrollRef.current = false;
      const targetY = detailScrollPositionRef.current || 0;
      const restore = () => {
        window.scrollTo(0, targetY);
        if (document.documentElement) document.documentElement.scrollTop = targetY;
        if (document.body) document.body.scrollTop = targetY;
      };
      restore();
      requestAnimationFrame(() => {
        restore();
        setTimeout(restore, 20);
        setTimeout(restore, 80);
      });
    }
  }, [isInstalling, isWaitingForPart, batchInstall, selectedTitleId]);
  if (isClosing) {
    return (
      <div className="min-h-screen bg-[#0a0a0f] text-white flex items-center justify-center px-4 font-ps5">
        <div className="max-w-lg w-full rounded-[2px] bg-[#141520] border border-white/10 p-8 text-center space-y-4">
          <div className="mx-auto w-14 h-14 rounded-full bg-emerald-600/20 border border-emerald-500/30 flex items-center justify-center text-emerald-400">
            <svg className="w-7 h-7" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
              <path d="M5 12l4 4L19 6" />
            </svg>
          </div>
          <h1 className="text-2xl font-bold">PKG Manager is closed</h1>
          <p className="text-sm text-zinc-300">The server process has stopped. You can close this tab.</p>
        </div>
      </div>
    );
  }

  if (isOffline) {
    return <OfflineScreen onRetry={() => {
      if (checkOnlineRef.current) checkOnlineRef.current();
      else window.location.reload();
    }} />;
  }

  // Initial loading splash to avoid any flash of buttons if reopening during install
  if (!initialStatusLoaded) {
    return <LoadingScreen />;
  }

  // Waiting for a disc or USB package part.
  if (isWaitingForPart) {
    return <WaitingForPartScreen
      installerStatus={installerStatus}
      isDiscSource={isDiscSource}
      onCancel={handleCancel}
    />;
  }

  // Active installation overlay.
  if (isInstalling) {
    return <InstallingScreen
      installerStatus={installerStatus}
      batchInstall={batchInstall}
      etaInfo={etaInfo}
      storage={storage}
      isDiscSource={isDiscSource}
      debugSpeeds={settings.pkg_install_debug && installerStatus?.pkg_path?.startsWith('live:') ? {
        upload: directUpload.uploadSpeed,
        install: speedCalcRef.current.speed
      } : null}
      onCancel={() => {
        handleCancel();
        if (installerStatus?.pkg_path?.startsWith('live:')) directUpload.cancel();
      }}
      packages={packages}
      directIconUrl={directUpload.iconUrl}
    />;
  }

  // Refresh and scan progress overlay (for full catalog rebuilds).
  if (scanStatus.is_scanning) {
    return <ScanningScreen scanStatus={scanStatus} />;
  }

  // Main application view.
  return (
    <div className="min-h-screen bg-[#0a0a0f] text-white flex flex-col font-ps5">
      <Toast notification={notification} />

      <fieldset
        id="app-main-content"
        disabled={isAnyModalOpen}
        inert={isAnyModalOpen ? '' : undefined}
        aria-hidden={isAnyModalOpen ? 'true' : undefined}
        className={`border-0 m-0 p-0 min-w-0 w-full flex flex-col flex-1 ${isAnyModalOpen ? 'pointer-events-none select-none' : ''}`}
      >
        <Header
        appVersion={appVersion}
        storage={storage}
        showSettings={showSettings}
        showSmbPage={showSmbPage}
        onSettingsClick={() => {
          if (showDirectInstall) {
            if (directTransferActive && !window.confirm('A direct installation is in progress. Leave this page?')) return;
            setShowDirectInstall(false);
          }
          if (showSmbPage) {
            handleCloseSmb();
            return;
          }
          if (showSettings) {
            handleCloseSettings();
            return;
          }
          handleOpenSettings();
        }}
        onRescan={handleQuickRescan}
        refreshing={refreshing}
        selectedDrive={selectedDrive}
        onBackToDrives={handleBackToDrives}
      />

      {/* Main Container */}
      <main className="w-full px-4 py-4 flex-1 space-y-6">
        {showDirectInstall ? (
          <DirectInstallView
            onBack={handleCloseDirectInstall}
            up={directUpload}
            storage={storage}
            installerStatus={installerStatus}
            debugEnabled={Boolean(settings.pkg_install_debug)}
          />
        ) : showSmbPage ? (
          <SmbManagementView
            settings={settings}
            onBack={handleCloseSmb}
            onAdd={() => {
              setSmbEditIndex(-1);
              setSmbForm({
                id: '',
                label: '',
                server: '',
                port: 445,
                share: '',
                path: '',
                username: '',
                password: '',
                workgroup: 'WORKGROUP',
                enabled: true
              });
              setSmbTestResult(null);
              setShowSmbModal(true);
            }}
            onEdit={(idx, sh) => {
              setSmbEditIndex(idx);
              setSmbForm({ ...sh });
              setSmbTestResult(null);
              setShowSmbModal(true);
            }}
            onToggle={handleToggleSmbShare}
            onRemove={handleRemoveSmbShare}
            onTest={handleTestSmbConnection}
            testing={smbTesting}
            onInstall={handleInstall}
          />
        ) : showSettings ? (
          <SettingsView
            settings={settings}
            onSaveSettings={handleSaveSettings}
            onClose={handleCloseSettings}
            onOpenSmb={handleOpenSmb}
            onInstallShortcut={handleInstallShortcut}
            onCloseApp={async () => {
              if (!window.confirm('Close PKG Manager? This will stop its server process.')) return;
              try {
                const result = await closeManager();
                if (result?.success) setIsClosing(true);
                else showToast('PKG Manager did not accept the close request.', 'error');
              } catch (err) {
                showToast('Failed to close PKG Manager: ' + err.message, 'error');
              }
            }}
            installingShortcut={installingShortcut}
            cacheStats={cacheStats}
            loadingStats={loadingStats}
            onClearCache={() => setShowClearCacheModal(true)}
            leftoversData={leftoversData}
            scanningLeftovers={scanningLeftovers}
            onScanLeftovers={handleScanLeftovers}
            onDeleteLeftover={setSelectedLeftoverToDelete}
            showDonateQr={showDonateQr}
            setShowDonateQr={setShowDonateQr}
          />
        ) : selectedTitle ? (
          <TitleDetailView
            title={selectedTitle}
            onBack={handleBackToPackages}
            onInstall={handleInstall}
            onInstallBaseAndUpdate={handleInstallBaseAndUpdate}
            onOpenLeftoverCleanup={handleOpenLeftoverCleanupForTitle}
            installerStatus={installerStatus}
            storage={storage}
            settings={settings}
            drives={drives}
            selectedDrive={selectedDrive}
          />
        ) : selectedDrive && (settings.smb_shares || []).some((share) => share.id === selectedDrive.id && share.browse_only) ? (
          <SmbFileBrowser key={selectedDrive.id}
            share={settings.smb_shares.find((share) => share.id === selectedDrive.id)}
            onBack={handleBackToDrives} onInstall={handleInstall} />
        ) : selectedDrive ? (
          <PackageGridView
            page={packagePage}
            onPageChange={setPackagePage}
            groupedTitles={groupedTitles}
            searchQuery={searchQuery}
            onSearch={(q) => setSearchQuery(q)}
            sortBy={sortBy}
            onSort={(s) => setSortBy(s)}
            onOpenTitle={handleOpenTitle}
            selectedDrive={selectedDrive}
            onBack={handleBackToDrives}
            settings={settings}
            installerStatus={installerStatus}
            loadingPackages={loadingPackages}
            packages={packages}
          />
        ) : (
          <DrivesView
            drives={drives}
            storage={storage}
            onSelectDrive={handleSelectDrive}
            onDirectInstall={openDirectInstall}
            showDirectInstall={!isPlayStation}
            loadingDrives={loadingDrives}
            refreshAll={handleQuickRescan}
            onRescan={handleQuickRescan}
          />
        )}
      </main>

      <Footer appVersion={appVersion} />
      </fieldset>

      <DonateModal
        show={showDonateModal}
        onClose={handleCloseDonateModal}
        onNeverShow={handleNeverShowDonateModal}
        donateNeverNotice={donateNeverNotice}
      />

      <ClearCacheModal
        show={showClearCacheModal}
        cacheStats={cacheStats}
        onClose={() => setShowClearCacheModal(false)}
        onConfirm={handleClearCache}
        clearing={clearingCache}
      />

      <SmbShareModal
        show={showSmbModal}
        onClose={() => setShowSmbModal(false)}
        isEditing={smbEditIndex >= 0}
        form={smbForm}
        setForm={setSmbForm}
        testResult={smbTestResult}
        testing={smbTesting}
        onTest={() => handleTestSmbConnection(smbForm)}
        onSave={handleSaveSmbShare}
      />

      <DeleteLeftoverModal
        show={!!selectedLeftoverToDelete}
        item={selectedLeftoverToDelete}
        onClose={() => setSelectedLeftoverToDelete(null)}
        onConfirm={() => handleConfirmDeleteLeftover(selectedLeftoverToDelete)}
        deleting={deletingLeftover}
      />
    </div>
  );
}
