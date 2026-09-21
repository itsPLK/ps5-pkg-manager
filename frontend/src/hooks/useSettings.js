import { useState } from 'react';
import { getSettings, saveSettings, installShortcut as apiInstallShortcut } from '../api/settings';
import { ALL_SOURCES_DRIVE } from '../constants/config';

export function useSettings(props) {
  const showToast = props.showToast;
  const selectedDriveRef = props.selectedDriveRef;
  const setSelectedDrive = props.setSelectedDrive;
  const fetchPackagesForDrive = props.fetchPackagesForDrive;
  const fetchDrives = props.fetchDrives;
  const fetchStorage = props.fetchStorage;
  const setPackages = props.setPackages;

  const [settings, setSettings] = useState(() => {
    try {
      const saved = localStorage.getItem('pkgmgr_settings');
      if (saved) return JSON.parse(saved);
    } catch (e) {}
    return {
      move_installed_to_end: true,
      fade_installed_packages: true,
      all_sources_mode: false,
      pkg_install_debug: false,
      smb_shares: []
    };
  });
  const [showSettings, setShowSettings] = useState(false);
  const [showSmbPage, setShowSmbPage] = useState(false);
  const [installingShortcut, setInstallingShortcut] = useState(false);

  const fetchSettings = async () => {
    try {
      const data = await getSettings();
      setSettings((prev) => {
        const updated = { ...prev, ...data };
        try { localStorage.setItem('pkgmgr_settings', JSON.stringify(updated)); } catch (e) {}
        return updated;
      });
      if (data.all_sources_mode && !selectedDriveRef.current) {
        if (setSelectedDrive) setSelectedDrive(ALL_SOURCES_DRIVE);
        if (selectedDriveRef) selectedDriveRef.current = ALL_SOURCES_DRIVE;
        if (fetchPackagesForDrive) fetchPackagesForDrive(ALL_SOURCES_DRIVE);
      }
    } catch (err) {}
  };

  const handleSaveSettings = async (newSettings) => {
    setSettings(newSettings);
    try {
      localStorage.setItem('pkgmgr_settings', JSON.stringify(newSettings));
    } catch (e) {}

    if (newSettings.all_sources_mode && !selectedDriveRef.current) {
      if (setSelectedDrive) setSelectedDrive(ALL_SOURCES_DRIVE);
      if (selectedDriveRef) selectedDriveRef.current = ALL_SOURCES_DRIVE;
      if (fetchPackagesForDrive) fetchPackagesForDrive(ALL_SOURCES_DRIVE);
    } else if (!newSettings.all_sources_mode && selectedDriveRef.current && selectedDriveRef.current.id === '__all__') {
      if (setSelectedDrive) setSelectedDrive(null);
      if (selectedDriveRef) selectedDriveRef.current = null;
      if (setPackages) setPackages([]);
      if (fetchDrives) fetchDrives();
      if (fetchStorage) fetchStorage();
    }

    try {
      await saveSettings(newSettings);
      showToast('Settings saved', 'success');
    } catch (e) {
      showToast('Failed to save settings: ' + e.message, 'error');
    }
  };

  const handleInstallShortcut = async () => {
    setInstallingShortcut(true);
    try {
      const data = await apiInstallShortcut();
      if (data.success) {
        showToast('PKG Manager shortcut installed to Media tab!', 'success');
      } else {
        showToast('Failed to install home screen shortcut.', 'error');
      }
    } catch (e) {
      showToast('Shortcut install error: ' + e.message, 'error');
    } finally {
      setInstallingShortcut(false);
    }
  };

  return {
    settings,
    setSettings,
    showSettings,
    setShowSettings,
    showSmbPage,
    setShowSmbPage,
    installingShortcut,
    fetchSettings,
    handleSaveSettings,
    handleInstallShortcut
  };
}
