import { useState, useRef, useEffect, useMemo } from 'react';
import { pollStatus, installPackage, cancelInstall } from '../api/installer';
import { formatBytes, formatEta } from '../utils/formatters';
import { getInstallStorageOptions } from '../utils/installStorage';

export function useInstaller(props) {
  const showToast = props.showToast;
  const fetchStorage = props.fetchStorage;
  const fetchPackagesForDrive = props.fetchPackagesForDrive;
  const selectedDriveRef = props.selectedDriveRef;
  const selectedTitleIdRef = props.selectedTitleIdRef;
  const detailScrollPositionRef = props.detailScrollPositionRef;
  const shouldRestoreDetailScrollRef = props.shouldRestoreDetailScrollRef;
  const storage = props.storage;
  const selectedTitle = props.selectedTitle;

  const [batchInstall, setBatchInstall] = useState(() => {
    try {
      const saved = localStorage.getItem('pkg_batch_install');
      return saved ? JSON.parse(saved) : null;
    } catch (e) {
      return null;
    }
  });

  const [installerStatus, setInstallerStatus] = useState({
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
  });

  const [initialStatusLoaded, setInitialStatusLoaded] = useState(false);

  const batchInstallRef = useRef(batchInstall);
  useEffect(() => {
    batchInstallRef.current = batchInstall;
  }, [batchInstall]);

  const installerStatusRef = useRef(installerStatus);
  useEffect(() => {
    installerStatusRef.current = installerStatus;
  }, [installerStatus]);

  const wasInstallingRef = useRef(false);

  const speedCalcRef = useRef({
    lastBytes: 0,
    lastTime: 0,
    speed: 0
  });

  const isWaitingForPart =
    installerStatus.is_installing &&
    (installerStatus.waiting_for_disc || installerStatus.status === 'waiting_disc');
  const isBatchActive = !!(batchInstall && (installerStatus.is_installing || batchInstall.stage === 'update'));
  const isInstalling = (installerStatus.is_installing || isBatchActive) && !isWaitingForPart;

  const isDiscSource =
    (installerStatus.pkg_path || '').startsWith('/mnt/disc') ||
    (installerStatus.pkg_path || '').indexOf('/disc') !== -1 ||
    (installerStatus.pkg_path || '').indexOf('_disc') !== -1;

  const etaInfo = useMemo(() => {
    if (!installerStatus.is_installing) return null;
    const isBatch = !!(batchInstall && batchInstall.combinedTotal > 0);
    const total = isBatch ? batchInstall.combinedTotal : installerStatus.total_bytes;
    if (total <= 0) return null;

    let downloaded = installerStatus.downloaded_bytes;
    if (isBatch) {
      if (batchInstall.stage === 'base') {
        downloaded = Math.min(batchInstall.baseSize, installerStatus.downloaded_bytes);
      } else {
        downloaded = batchInstall.baseSize + Math.min(batchInstall.updateSize, installerStatus.downloaded_bytes);
      }
    }

    const remaining = Math.max(0, total - downloaded);
    const speed = speedCalcRef.current.speed;
    if (!speed || speed < 1024) {
      return { text: 'Calculating ETA...', speedStr: '' };
    }
    const seconds = remaining / speed;
    return {
      text: formatEta(seconds) || 'Calculating ETA...',
      speedStr: `${formatBytes(speed)}/s`
    };
  }, [installerStatus.is_installing, installerStatus.downloaded_bytes, installerStatus.total_bytes, batchInstall]);

  const fetchStatus = async () => {
    try {
      const data = await pollStatus();
      if (data) {
        if (data.is_installing && installerStatusRef.current && !installerStatusRef.current.is_installing && selectedTitleIdRef && selectedTitleIdRef.current) {
          if (shouldRestoreDetailScrollRef && !shouldRestoreDetailScrollRef.current) {
            const currentY = window.scrollY || window.pageYOffset || (document.documentElement && document.documentElement.scrollTop) || (document.body && document.body.scrollTop) || 0;
            if (detailScrollPositionRef) detailScrollPositionRef.current = currentY;
            shouldRestoreDetailScrollRef.current = true;
          }
        }
        const currentBatch = batchInstallRef.current;
        const isBatchUpdate = currentBatch && currentBatch.stage === 'base' &&
          (data.pkg_path === currentBatch.updatePkg.path || data.pkg_kind === 'update');
        if (isBatchUpdate && data.is_installing) {
          const nextBatch = { ...currentBatch, stage: 'update' };
          try {
            localStorage.setItem('pkg_batch_install', JSON.stringify(nextBatch));
          } catch (e) {}
          setBatchInstall(nextBatch);
        } else if (isBatchUpdate && !data.is_installing && (data.completed || data.failed)) {
          /* The page may be reopened after the native batch already
           * finished, so do not rely on a prior in-page poll transition. */
          try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
          setBatchInstall(null);
        }

        setInstallerStatus(data);
        setInitialStatusLoaded(true);

        const now = Date.now();
        if (data.is_installing && data.downloaded_bytes > 0) {
          if (speedCalcRef.current.lastTime > 0) {
            const timeDiffSec = (now - speedCalcRef.current.lastTime) / 1000;
            if (timeDiffSec >= 1.0) {
              const bytesDiff = data.downloaded_bytes - speedCalcRef.current.lastBytes;
              if (bytesDiff >= 0) {
                const instantSpeed = bytesDiff / timeDiffSec;
                speedCalcRef.current.speed = speedCalcRef.current.speed === 0
                  ? instantSpeed
                  : (speedCalcRef.current.speed * 0.7 + instantSpeed * 0.3);
              }
              speedCalcRef.current.lastBytes = data.downloaded_bytes;
              speedCalcRef.current.lastTime = now;
            }
          } else {
            speedCalcRef.current.lastBytes = data.downloaded_bytes;
            speedCalcRef.current.lastTime = now;
          }
        } else {
          speedCalcRef.current.lastBytes = 0;
          speedCalcRef.current.lastTime = 0;
          speedCalcRef.current.speed = 0;
        }

        if (wasInstallingRef.current && !data.is_installing && (data.completed || data.failed)) {
          if (fetchStorage) fetchStorage();
          if (selectedDriveRef && selectedDriveRef.current && fetchPackagesForDrive) {
            fetchPackagesForDrive(selectedDriveRef.current);
          }
        }

        const currentBatchAfterStatus = batchInstallRef.current;
        if (currentBatchAfterStatus) {
          if (currentBatchAfterStatus.stage === 'update' && wasInstallingRef.current && !data.is_installing && (data.completed || data.failed)) {
            try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
            setBatchInstall(null);
            if (data.completed) {
              if (showToast) showToast(`Base + Update installed for ${currentBatchAfterStatus.titleName}!`, 'success');
            }
          } else if (data.failed && !data.is_installing) {
            try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
            setBatchInstall(null);
          }
        }

        wasInstallingRef.current = data.is_installing;
      }
    } catch (err) {}
  };

  const handleInstall = async (pkg) => {
    if (installerStatus.is_installing) {
      if (showToast) showToast('Another installation is already in progress', 'warning');
      return;
    }

    try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
    setBatchInstall(null);

    // PS4 packages may install to Internal, M.2, or USB extended storage.
    // PS5 packages may install only to Internal or M.2.
    const storageOptions = getInstallStorageOptions(storage, pkg.title_id || selectedTitle?.title_id);
    const maxAvailable = storageOptions.reduce((max, option) => Math.max(max, option.free), 0);
    const requiredSpace = Number(pkg.total_pkg_size || pkg.file_size) || 0;

    if (storage && maxAvailable < requiredSpace) {
      if (showToast) {
        const available = storageOptions.map((option) => `${option.label} (${formatBytes(option.free)})`).join(' or ');
        showToast(`Insufficient storage! Needs ${formatBytes(requiredSpace)}, but none of these locations has enough space: ${available}.`, 'error');
      }
      return;
    }

    if (selectedTitleIdRef && selectedTitleIdRef.current) {
      const currentY = window.scrollY || window.pageYOffset || (document.documentElement && document.documentElement.scrollTop) || (document.body && document.body.scrollTop) || 0;
      if (detailScrollPositionRef) detailScrollPositionRef.current = currentY;
      if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = true;
    }

    speedCalcRef.current = { lastBytes: 0, lastTime: 0, speed: 0 };
    if (showToast) showToast(`Starting installation for ${pkg.title_name || 'package'}...`, 'info');

    try {
      const data = await installPackage(pkg.path);
      if (!data || !data.success) {
        if (showToast) showToast(data.error || 'Failed to start installation', 'error');
        if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = false;
      } else {
        if (showToast) showToast(`Installing ${pkg.title_name || 'package'}...`, 'success');
        fetchStatus();
      }
    } catch (err) {
      if (showToast) showToast('Install request failed: ' + err.message, 'error');
      if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = false;
    }
  };

  const handleInstallBaseAndUpdate = async (basePkg, updatePkg) => {
    if (installerStatus.is_installing) {
      if (showToast) showToast('Another installation is already in progress', 'warning');
      return;
    }

    const baseRequired = Number(basePkg.total_pkg_size || basePkg.file_size) || 0;
    const updateRequired = Number(updatePkg.total_pkg_size || updatePkg.file_size) || 0;
    const combinedSpace = baseRequired + updateRequired;
    const storageOptions = getInstallStorageOptions(
      storage,
      updatePkg.title_id || basePkg.title_id || selectedTitle?.title_id
    );
    const maxAvailable = storageOptions.reduce((max, option) => Math.max(max, option.free), 0);

    if (storage && maxAvailable < combinedSpace) {
      if (showToast) {
        const available = storageOptions.map((option) => `${option.label} (${formatBytes(option.free)})`).join(' or ');
        showToast(`Insufficient storage! Needs ${formatBytes(combinedSpace)}, but none of these locations has enough space: ${available}.`, 'error');
      }
      return;
    }

    if (selectedTitleIdRef && selectedTitleIdRef.current) {
      const currentY = window.scrollY || window.pageYOffset || (document.documentElement && document.documentElement.scrollTop) || (document.body && document.body.scrollTop) || 0;
      if (detailScrollPositionRef) detailScrollPositionRef.current = currentY;
      if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = true;
    }

    const batch = {
      stage: 'base',
      basePkg,
      updatePkg,
      baseSize: baseRequired,
      updateSize: updateRequired,
      combinedTotal: combinedSpace,
      titleName: (selectedTitle && selectedTitle.title_name) ? selectedTitle.title_name : (basePkg.title_name || 'Title'),
      titleId: (selectedTitle && selectedTitle.title_id) ? selectedTitle.title_id : (basePkg.title_id || ''),
      iconPath: (selectedTitle && selectedTitle.iconPath) ? selectedTitle.iconPath : basePkg.path
    };

    try {
      localStorage.setItem('pkg_batch_install', JSON.stringify(batch));
    } catch (e) {}
    setBatchInstall(batch);

    speedCalcRef.current = { lastBytes: 0, lastTime: 0, speed: 0 };
    if (showToast) showToast(`Starting Base + Update install for ${batch.titleName}...`, 'info');

    try {
      const data = await installPackage(basePkg.path, updatePkg.path);
      if (!data || !data.success) {
        if (showToast) showToast(data.error || 'Failed to start base installation', 'error');
        try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
        setBatchInstall(null);
        if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = false;
      } else {
        fetchStatus();
      }
    } catch (err) {
      if (showToast) showToast('Install request failed: ' + err.message, 'error');
      try { localStorage.removeItem('pkg_batch_install'); } catch (e) {}
      setBatchInstall(null);
      if (shouldRestoreDetailScrollRef) shouldRestoreDetailScrollRef.current = false;
    }
  };

  const handleCancel = async () => {
    try {
      localStorage.removeItem('pkg_batch_install');
    } catch (e) {}
    setBatchInstall(null);

    try {
      const data = await cancelInstall();
      if (data.success) {
        if (showToast) showToast('Installation canceled', 'info');
        fetchStatus();
      } else {
        if (showToast) showToast(data.error || 'Failed to cancel installation', 'error');
      }
    } catch (err) {
      if (showToast) showToast('Cancel request failed: ' + err.message, 'error');
    }
  };

  return {
    installerStatus,
    setInstallerStatus,
    batchInstall,
    setBatchInstall,
    initialStatusLoaded,
    setInitialStatusLoaded,
    etaInfo,
    isWaitingForPart,
    isBatchActive,
    isInstalling,
    isDiscSource,
    speedCalcRef,
    wasInstallingRef,
    batchInstallRef,
    installerStatusRef,
    fetchStatus,
    handleInstall,
    handleInstallBaseAndUpdate,
    handleCancel
  };
}
