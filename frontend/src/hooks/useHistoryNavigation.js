import { useEffect, useRef, useCallback } from 'react';
import { ALL_SOURCES_DRIVE } from '../constants/config';

export function getRouteFromHash(hash) {
  const clean = (hash || '').replace(/^#\/?/, '').trim();
  if (!clean) return { type: 'drives' };
  if (clean === 'settings') return { type: 'settings' };
  if (clean === 'smb') return { type: 'smb' };
  if (clean === 'direct-install') return { type: 'direct-install' };
  if (clean.startsWith('drive/')) {
    const driveId = decodeURIComponent(clean.slice(6));
    return { type: 'drive', driveId };
  }
  if (clean.startsWith('title/')) {
    const titleId = decodeURIComponent(clean.slice(6));
    return { type: 'title', titleId };
  }
  return { type: 'drives' };
}

export function formatHash(route) {
  if (!route || route.type === 'drives') return '#/';
  if (route.type === 'settings') return '#/settings';
  if (route.type === 'smb') return '#/smb';
  if (route.type === 'direct-install') return '#/direct-install';
  if (route.type === 'drive') return `#/drive/${encodeURIComponent(route.driveId || '__all__')}`;
  if (route.type === 'title') return `#/title/${encodeURIComponent(route.titleId)}`;
  return '#/';
}

export function writeHistory(route, replace = false) {
  if (!window.history || !window.history.pushState) return;
  const hash = formatHash(route);
  if (window.location.hash === hash) return;
  const state = { ...route, hash };
  try {
    if (replace && window.history.replaceState) {
      window.history.replaceState(state, '', hash);
    } else {
      window.history.pushState(state, '', hash);
    }
  } catch (e) {}
}

export function getHistoryChain(route) {
  if (!route || route.type === 'drives') {
    return [{ type: 'drives' }];
  }
  if (route.type === 'drive') {
    return [
      { type: 'drives' },
      route
    ];
  }
  if (route.type === 'title') {
    return [
      { type: 'drives' },
      { type: 'drive', driveId: route.driveId || '__all__' },
      route
    ];
  }
  if (route.type === 'settings') {
    return [
      { type: 'drives' },
      route
    ];
  }
  if (route.type === 'smb') {
    return [
      { type: 'drives' },
      { type: 'settings' },
      route
    ];
  }
  if (route.type === 'direct-install') {
    return [
      { type: 'drives' },
      route
    ];
  }
  return [{ type: 'drives' }];
}

export function seedHistory(route) {
  if (!window.history || !window.history.pushState || !window.history.replaceState) return;

  // Reuse the current document entry for the app root. Adding a second, hashless
  // baseline entry here makes Back from the root require another history.back(),
  // which corrupts the browser's Forward stack when the app is re-entered.
  const chain = getHistoryChain(route);
  for (let i = 0; i < chain.length; i++) {
    const r = chain[i];
    const hash = formatHash(r);
    const state = { ...r, hash };
    try {
      if (i === 0 && window.history.replaceState) {
        window.history.replaceState(state, '', hash);
      } else {
        window.history.pushState(state, '', hash);
      }
    } catch (e) {}
  }
}

export function useHistoryNavigation(props) {
  const {
    setSelectedDrive,
    selectedDriveRef,
    setSelectedTitleId,
    selectedTitleIdRef,
    showSettings,
    setShowSettings,
    showSmbPage,
    setShowSmbPage,
    showDirectInstall,
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
    initialRoute,
  } = props;

  const currentRouteRef = useRef({ type: 'drives' });
  const initialRouteRef = useRef(initialRoute || { type: 'drives' });
  const initialRouteAppliedRef = useRef(false);
  const historySeededRef = useRef(false);

  const drivesRef = useRef(drives);
  useEffect(() => {
    drivesRef.current = drives;
  }, [drives]);

  const showSettingsRef = useRef(showSettings);
  showSettingsRef.current = showSettings;
  useEffect(() => {
    showSettingsRef.current = showSettings;
  }, [showSettings]);

  const showSmbPageRef = useRef(showSmbPage);
  showSmbPageRef.current = showSmbPage;
  useEffect(() => {
    showSmbPageRef.current = showSmbPage;
  }, [showSmbPage]);

  const showDirectInstallRef = useRef(showDirectInstall);
  const directTransferActiveRef = useRef(directTransferActive);
  directTransferActiveRef.current = directTransferActive;
  showDirectInstallRef.current = showDirectInstall;
  useEffect(() => {
    showDirectInstallRef.current = showDirectInstall;
  }, [showDirectInstall]);

  const isInstallingRef = useRef(false);
  isInstallingRef.current = Boolean(
    installerStatus?.is_installing ||
    installerStatus?.waiting_for_disc ||
    isBatchActive
  );
  useEffect(() => {
    isInstallingRef.current = Boolean(
      installerStatus?.is_installing ||
      installerStatus?.waiting_for_disc ||
      isBatchActive
    );
  }, [installerStatus?.is_installing, installerStatus?.waiting_for_disc, isBatchActive]);

  const modalStateRef = useRef({});
  modalStateRef.current = {
    showDonateModal,
    handleCloseDonateModal,
    showClearCacheModal,
    setShowClearCacheModal,
    showSmbModal,
    setShowSmbModal,
    selectedLeftoverToDelete,
    setSelectedLeftoverToDelete,
  };
  useEffect(() => {
    modalStateRef.current = {
      showDonateModal,
      handleCloseDonateModal,
      showClearCacheModal,
      setShowClearCacheModal,
      showSmbModal,
      setShowSmbModal,
      selectedLeftoverToDelete,
      setSelectedLeftoverToDelete,
    };
  }, [
    showDonateModal,
    handleCloseDonateModal,
    showClearCacheModal,
    setShowClearCacheModal,
    showSmbModal,
    setShowSmbModal,
    selectedLeftoverToDelete,
    setSelectedLeftoverToDelete,
  ]);

  const getActiveViewType = useCallback(() => {
    if (showDirectInstallRef.current) return 'direct-install';
    if (showSmbPageRef.current) return 'smb';
    if (showSettingsRef.current) return 'settings';
    if (selectedTitleIdRef.current) return 'title';
    if (selectedDriveRef.current) return 'drive';
    return 'drives';
  }, [selectedDriveRef, selectedTitleIdRef]);

  // Seed history on mount
  useEffect(() => {
    if (historySeededRef.current) return;
    historySeededRef.current = true;
    const hashRoute = getRouteFromHash(window.location.hash);
    const startRoute = (hashRoute.type !== 'drives') ? hashRoute : (initialRoute || { type: 'drives' });
    currentRouteRef.current = startRoute;
    seedHistory(startRoute);
  }, []);

  // Restore a deep link on the first load. The history seed above only
  // creates browser entries; it does not select the corresponding view.
  // Package data can arrive after this hook mounts, so apply the route once
  // from the current drive list and let the normal render update handle the
  // package detail itself.
  useEffect(() => {
    if (initialRouteAppliedRef.current) return;
    const hashRoute = getRouteFromHash(window.location.hash);
    const route = hashRoute.type !== 'drives' ? hashRoute : initialRouteRef.current;
    const driveRoute = route.type === 'title'
      ? { type: 'drive', driveId: route.driveId || '__all__' }
      : route;

    if (driveRoute.type === 'drive') {
      const driveId = driveRoute.driveId || '__all__';
      const drive = driveId === '__all__'
        ? ALL_SOURCES_DRIVE
        : (drivesRef.current.find((d) => (d.id || d.path) === driveId) || {
            id: driveId, path: driveId, label: driveId, clickable: true
          });
      setShowSettings(false);
      setShowSmbPage(false);
      setShowDirectInstall(false);
      setSelectedDrive(drive);
      selectedDriveRef.current = drive;
      if (fetchPackagesForDrive) fetchPackagesForDrive(drive);
      if (route.type === 'title') {
        setSelectedTitleId(route.titleId);
        selectedTitleIdRef.current = route.titleId;
      }
    } else if (route.type === 'settings') {
      setShowSettings(true);
      setShowSmbPage(false);
      setShowDirectInstall(false);
      if (fetchCacheStats) fetchCacheStats();
    } else if (route.type === 'smb') {
      setShowSettings(false);
      setShowSmbPage(true);
      setShowDirectInstall(false);
    } else if (route.type === 'direct-install') {
      setShowSettings(false);
      setShowSmbPage(false);
      setShowDirectInstall(true);
    } else {
      setShowSettings(false);
      setShowSmbPage(false);
      setShowDirectInstall(false);
      setSelectedDrive(null);
      selectedDriveRef.current = null;
      setSelectedTitleId(null);
      selectedTitleIdRef.current = null;
      if (setPackages) setPackages([]);
      if (setSearchQuery) setSearchQuery('');
    }
    initialRouteAppliedRef.current = true;
  }, [
    drives,
    fetchCacheStats,
    fetchPackagesForDrive,
    setPackages,
    setSearchQuery,
    setSelectedDrive,
    setSelectedTitleId,
    setShowDirectInstall,
    setShowSettings,
    setShowSmbPage,
  ]);

  // Listen to popstate (triggered by controller Circle button or browser back/forward)
  useEffect(() => {
    const handlePopState = () => {
      if (showDirectInstallRef.current && directTransferActiveRef.current) {
        if (!window.confirm('A direct installation is in progress. Leave this page?')) {
          writeHistory({ type: 'direct-install' }, false);
          return;
        }
      }
      // 1. If install/stream task is in progress, close the PS5 browser on Circle press
      if (isInstallingRef.current && !directTransferActiveRef.current) {
        try {
          window.close();
        } catch (e) {}
        try {
          window.history.back();
        } catch (e) {}
        return;
      }

      // 2. Close modal if any modal dialog is currently open
      const m = modalStateRef.current;
      const anyModal = Boolean(
        m.showDonateModal ||
        m.showClearCacheModal ||
        m.showSmbModal ||
        m.selectedLeftoverToDelete
      );
      if (anyModal) {
        if (m.showDonateModal && m.handleCloseDonateModal) m.handleCloseDonateModal();
        if (m.showClearCacheModal && m.setShowClearCacheModal) m.setShowClearCacheModal(false);
        if (m.showSmbModal && m.setShowSmbModal) m.setShowSmbModal(false);
        if (m.selectedLeftoverToDelete && m.setSelectedLeftoverToDelete) m.setSelectedLeftoverToDelete(null);

        // Re-push current route so we remain on the current page in history
        writeHistory(currentRouteRef.current, false);
        return;
      }

      // 3. Read the destination before deciding whether this is a request to
      // leave the app. A popstate is emitted for both Back and Forward, so
      // looking only at the view that was rendered before the pop misclassifies
      // Forward from the root view as another Back and can walk into the page
      // that opened the app.
      const target = getRouteFromHash(window.location.hash);
      const currentView = getActiveViewType();

      // If the user was ALREADY on the root list of storage media (DrivesView)
      // and pressed Circle / Back, close the PS5 browser!
      // A Forward navigation from the root has a non-root target and must be
      // handled as a normal route transition below.
      if (currentView === 'drives' && target.type === 'drives') {
        try {
          window.close();
        } catch (e) {}
        try {
          window.history.back();
        } catch (e) {}
        return;
      }

      // 4. Normal view transition from popped history
      currentRouteRef.current = target;

      if (target.type === 'smb') {
        setShowSettings(false);
        setShowSmbPage(true);
        setShowDirectInstall(false);
      } else if (target.type === 'direct-install') {
        setShowSmbPage(false);
        setShowSettings(false);
        setShowDirectInstall(true);
      } else if (target.type === 'settings') {
        setShowSmbPage(false);
        setShowSettings(true);
        setShowDirectInstall(false);
        if (fetchCacheStats) fetchCacheStats();
      } else if (target.type === 'title') {
        setShowSettings(false);
        setShowSmbPage(false);
        setShowDirectInstall(false);
        setSelectedTitleId(target.titleId);
        selectedTitleIdRef.current = target.titleId;
      } else if (target.type === 'drive') {
        setShowSettings(false);
        setShowSmbPage(false);
        setShowDirectInstall(false);
        if (selectedTitleIdRef.current) {
          const currentY = window.scrollY || window.pageYOffset || document.documentElement.scrollTop;
          detailScrollPositionRef.current = currentY;
          shouldRestoreScrollRef.current = true;
          setSelectedTitleId(null);
          selectedTitleIdRef.current = null;
        }
        if (!selectedDriveRef.current || (selectedDriveRef.current.id !== target.driveId && selectedDriveRef.current.path !== target.driveId)) {
          if (target.driveId === '__all__') {
            setSelectedDrive(ALL_SOURCES_DRIVE);
            selectedDriveRef.current = ALL_SOURCES_DRIVE;
            if (fetchPackagesForDrive) fetchPackagesForDrive(ALL_SOURCES_DRIVE);
          } else {
            const found = drivesRef.current?.find((d) => (d.id || d.path) === target.driveId);
            const driveToSet = found || { id: target.driveId, path: target.driveId, label: target.driveId, clickable: true };
            setSelectedDrive(driveToSet);
            selectedDriveRef.current = driveToSet;
            if (fetchPackagesForDrive) fetchPackagesForDrive(driveToSet);
          }
        }
      } else {
        // 'drives'
        setShowSettings(false);
        setShowSmbPage(false);
        setShowDirectInstall(false);
        setSelectedTitleId(null);
        selectedTitleIdRef.current = null;
        setSelectedDrive(null);
        selectedDriveRef.current = null;
        if (setPackages) setPackages([]);
        if (setSearchQuery) setSearchQuery('');
        if (fetchDrives) fetchDrives();
        if (fetchStorage) fetchStorage();
      }
    };

    window.addEventListener('popstate', handlePopState);
    return () => window.removeEventListener('popstate', handlePopState);
  }, [
    fetchCacheStats,
    fetchDrives,
    fetchPackagesForDrive,
    fetchStorage,
    getActiveViewType,
    setPackages,
    setSearchQuery,
    setSelectedDrive,
    setSelectedTitleId,
    setShowSettings,
    setShowSmbPage,
    setShowDirectInstall,
    showToast,
    detailScrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    shouldRestoreScrollRef,
  ]);

  const handleSelectDrive = useCallback((drive) => {
    if (!drive || !drive.clickable) return;
    scrollPositionRef.current = 0;
    detailScrollPositionRef.current = 0;
    shouldRestoreDetailScrollRef.current = false;
    if (setSearchQuery) setSearchQuery('');
    setSelectedDrive(drive);
    selectedDriveRef.current = drive;
    setSelectedTitleId(null);
    selectedTitleIdRef.current = null;
    if (fetchPackagesForDrive) fetchPackagesForDrive(drive);
    if (triggerQuickScan) triggerQuickScan(drive);
    window.scrollTo(0, 0);
    const route = { type: 'drive', driveId: drive.id || drive.path };
    currentRouteRef.current = route;
    writeHistory(route, false);
  }, [
    fetchPackagesForDrive,
    scrollPositionRef,
    detailScrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    setSelectedDrive,
    setSelectedTitleId,
    setSearchQuery,
    shouldRestoreDetailScrollRef,
    triggerQuickScan,
  ]);

  const handleBackToDrives = useCallback(() => {
    if (window.history && selectedDriveRef.current) {
      window.history.back();
    } else {
      scrollPositionRef.current = 0;
      detailScrollPositionRef.current = 0;
      shouldRestoreDetailScrollRef.current = false;
      setSelectedDrive(null);
      selectedDriveRef.current = null;
      setSelectedTitleId(null);
      selectedTitleIdRef.current = null;
      if (setPackages) setPackages([]);
      if (setSearchQuery) setSearchQuery('');
      if (fetchDrives) fetchDrives();
      if (fetchStorage) fetchStorage();
      window.scrollTo(0, 0);
      const route = { type: 'drives' };
      currentRouteRef.current = route;
      writeHistory(route, true);
    }
  }, [
    detailScrollPositionRef,
    fetchDrives,
    fetchStorage,
    scrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    setPackages,
    setSearchQuery,
    setSelectedDrive,
    setSelectedTitleId,
    shouldRestoreDetailScrollRef,
  ]);

  const handleOpenTitle = useCallback((titleId) => {
    const currentY = window.scrollY || window.pageYOffset || document.documentElement.scrollTop;
    scrollPositionRef.current = currentY;
    shouldRestoreScrollRef.current = true;
    detailScrollPositionRef.current = 0;
    setSelectedTitleId(titleId);
    selectedTitleIdRef.current = titleId;
    window.scrollTo(0, 0);
    const driveId = selectedDriveRef.current?.id || '__all__';
    const route = { type: 'title', titleId, driveId };
    currentRouteRef.current = route;
    writeHistory(route, false);
  }, [
    detailScrollPositionRef,
    scrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    setSelectedTitleId,
    shouldRestoreScrollRef,
  ]);

  const handleBackToPackages = useCallback(() => {
    if (window.history && selectedTitleIdRef.current) {
      window.history.back();
    } else {
      const currentY = window.scrollY || window.pageYOffset || document.documentElement.scrollTop;
      detailScrollPositionRef.current = currentY;
      shouldRestoreDetailScrollRef.current = true;
      setSelectedTitleId(null);
      selectedTitleIdRef.current = null;
      const driveId = selectedDriveRef.current?.id || '__all__';
      const route = { type: 'drive', driveId };
      currentRouteRef.current = route;
      writeHistory(route, true);
    }
  }, [
    detailScrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    setSelectedTitleId,
    shouldRestoreDetailScrollRef,
  ]);

  const handleOpenSettings = useCallback(() => {
    if (fetchCacheStats) fetchCacheStats();
    setShowSettings(true);
    const route = { type: 'settings' };
    currentRouteRef.current = route;
    writeHistory(route, false);
  }, [fetchCacheStats, setShowSettings]);

  const handleCloseSettings = useCallback(() => {
    if (window.history && showSettingsRef.current) {
      window.history.back();
    } else {
      setShowSettings(false);
      setShowSmbPage(false);
      let prevRoute = { type: 'drives' };
      if (selectedTitleIdRef.current) {
        prevRoute = { type: 'title', titleId: selectedTitleIdRef.current, driveId: selectedDriveRef.current?.id || '__all__' };
      } else if (selectedDriveRef.current) {
        prevRoute = { type: 'drive', driveId: selectedDriveRef.current.id || '__all__' };
      }
      currentRouteRef.current = prevRoute;
      writeHistory(prevRoute, true);
    }
  }, [selectedDriveRef, selectedTitleIdRef, setShowSettings, setShowSmbPage]);

  const handleOpenSmb = useCallback(() => {
    setShowSmbPage(true);
    const route = { type: 'smb' };
    currentRouteRef.current = route;
    writeHistory(route, false);
  }, [setShowSmbPage]);

  const handleCloseSmb = useCallback(() => {
    if (window.history && showSmbPageRef.current) {
      window.history.back();
    } else {
      setShowSmbPage(false);
      setShowSettings(true);
      const route = { type: 'settings' };
      currentRouteRef.current = route;
      writeHistory(route, true);
    }
  }, [setShowSettings, setShowSmbPage]);

  const handleOpenDirectInstall = useCallback(() => {
    setShowSmbPage(false);
    setShowSettings(false);
    setShowDirectInstall(true);
    window.scrollTo(0, 0);
    const route = { type: 'direct-install' };
    currentRouteRef.current = route;
    writeHistory(route, false);
  }, [setShowDirectInstall, setShowSettings, setShowSmbPage]);

  const handleCloseDirectInstall = useCallback(() => {
    if (directTransferActiveRef.current &&
        !window.confirm('A direct installation is in progress. Leave this page?')) {
      return;
    }
    setShowDirectInstall(false);
    setShowSettings(false);
    setShowSmbPage(false);
    setSelectedDrive(null);
    selectedDriveRef.current = null;
    setSelectedTitleId(null);
    selectedTitleIdRef.current = null;
    if (setPackages) setPackages([]);
    if (setSearchQuery) setSearchQuery('');
    if (fetchDrives) fetchDrives();
    if (fetchStorage) fetchStorage();
    scrollPositionRef.current = 0;
    detailScrollPositionRef.current = 0;
    shouldRestoreDetailScrollRef.current = false;
    currentRouteRef.current = { type: 'drives' };
    writeHistory({ type: 'drives' }, true);
    window.scrollTo(0, 0);
  }, [
    detailScrollPositionRef,
    fetchDrives,
    fetchStorage,
    scrollPositionRef,
    selectedDriveRef,
    selectedTitleIdRef,
    setPackages,
    setSearchQuery,
    setSelectedDrive,
    setSelectedTitleId,
    setShowDirectInstall,
    setShowSettings,
    setShowSmbPage,
    shouldRestoreDetailScrollRef,
  ]);

  return {
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
  };
}
