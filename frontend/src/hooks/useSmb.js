import { useState } from 'react';
import { testSmb } from '../api/smb';

export function useSmb(props) {
  const settings = props.settings;
  const showToast = props.showToast;
  const handleSaveSettings = props.handleSaveSettings;
  const refreshAll = props.refreshAll;

  const [showSmbModal, setShowSmbModal] = useState(false);
  const [smbEditIndex, setSmbEditIndex] = useState(-1);
  const [smbForm, setSmbForm] = useState({
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
  const [smbTesting, setSmbTesting] = useState(false);
  const [smbTestResult, setSmbTestResult] = useState(null);

  const handleSaveSmbShare = async () => {
    const rawServer = (smbForm.server || '').trim();
    const rawShare = (smbForm.share || '').trim();
    const rawPath = (smbForm.path || '').trim();

    let cleanServer = rawServer.replace(/^smb:\/\/+/i, '').replace(/^[\\/]+|[\\/]+$/g, '');
    let cleanShare = rawShare.replace(/^[\\/]+|[\\/]+$/g, '');
    let cleanPath = rawPath.replace(/^[\\/]+|[\\/]+$/g, '').replace(/\\/g, '/');

    if (cleanServer.includes('/') || cleanServer.includes('\\')) {
      const parts = cleanServer.split(/[\\/]+/).filter(Boolean);
      cleanServer = parts[0] || '';
      if (!cleanShare && parts.length > 1) {
        cleanShare = parts[1];
      }
      if (parts.length > 2) {
        const subpath = parts.slice(2).join('/');
        cleanPath = cleanPath ? `${subpath}/${cleanPath}` : subpath;
      }
    }

    let port = parseInt(smbForm.port, 10) || 445;
    const firstColon = cleanServer.indexOf(':');
    const lastColon = cleanServer.lastIndexOf(':');
    let portColon = -1;

    if (firstColon > 0 && firstColon === lastColon) {
      portColon = firstColon;
    } else if (firstColon > 0 && firstColon !== lastColon) {
      const bracketIdx = cleanServer.lastIndexOf(']');
      if (bracketIdx > 0 && lastColon > bracketIdx) {
        portColon = lastColon;
      }
    }

    if (portColon > 0) {
      const portCandidate = cleanServer.slice(portColon + 1);
      if (/^\d+$/.test(portCandidate)) {
        port = parseInt(portCandidate, 10);
        cleanServer = cleanServer.slice(0, portColon);
      }
    }

    if (!cleanServer || !cleanShare) {
      showToast('Server and Share name are required', 'error');
      return;
    }
    const currentShares = Array.isArray(settings.smb_shares) ? [...settings.smb_shares] : [];
    const formCopy = {
      ...smbForm,
      server: cleanServer,
      port,
      share: cleanShare,
      path: cleanPath,
      username: (smbForm.username || '').trim(),
      workgroup: (smbForm.workgroup || '').trim() || 'WORKGROUP'
    };
    if (!formCopy.label.trim()) {
      formCopy.label = `${cleanServer}/${cleanShare}`;
    }
    if (!formCopy.id) {
      formCopy.id = 'smb_' + Date.now().toString(36);
    }
    if (smbEditIndex >= 0 && smbEditIndex < currentShares.length) {
      currentShares[smbEditIndex] = formCopy;
    } else {
      currentShares.push(formCopy);
    }
    const newSettings = { ...settings, smb_shares: currentShares };
    await handleSaveSettings(newSettings);
    setShowSmbModal(false);
    setSmbTestResult(null);
    if (refreshAll) refreshAll();
  };

  const handleRemoveSmbShare = async (idx) => {
    const currentShares = Array.isArray(settings.smb_shares) ? [...settings.smb_shares] : [];
    currentShares.splice(idx, 1);
    const newSettings = { ...settings, smb_shares: currentShares };
    await handleSaveSettings(newSettings);
    if (refreshAll) refreshAll();
  };

  const handleToggleSmbShare = async (idx) => {
    const currentShares = Array.isArray(settings.smb_shares) ? [...settings.smb_shares] : [];
    if (currentShares[idx]) {
      currentShares[idx] = { ...currentShares[idx], enabled: !currentShares[idx].enabled };
      const newSettings = { ...settings, smb_shares: currentShares };
      await handleSaveSettings(newSettings);
      if (refreshAll) refreshAll();
    }
  };

  const handleTestSmbConnection = async (shareCfg) => {
    if (!shareCfg) {
      showToast('No share configuration provided', 'error');
      return;
    }

    const rawServer = (shareCfg.server || '').trim();
    const rawShare = (shareCfg.share || '').trim();
    const rawPath = (shareCfg.path || '').trim();

    let cleanServer = rawServer.replace(/^smb:\/\/+/i, '').replace(/^[\\/]+|[\\/]+$/g, '');
    let cleanShare = rawShare.replace(/^[\\/]+|[\\/]+$/g, '');
    let cleanPath = rawPath.replace(/^[\\/]+|[\\/]+$/g, '').replace(/\\/g, '/');

    if (cleanServer.includes('/') || cleanServer.includes('\\')) {
      const parts = cleanServer.split(/[\\/]+/).filter(Boolean);
      cleanServer = parts[0] || '';
      if (!cleanShare && parts.length > 1) {
        cleanShare = parts[1];
      }
      if (parts.length > 2) {
        const subpath = parts.slice(2).join('/');
        cleanPath = cleanPath ? `${subpath}/${cleanPath}` : subpath;
      }
    }

    let port = parseInt(shareCfg.port, 10) || 445;
    const firstColon = cleanServer.indexOf(':');
    const lastColon = cleanServer.lastIndexOf(':');
    let portColon = -1;

    if (firstColon > 0 && firstColon === lastColon) {
      portColon = firstColon;
    } else if (firstColon > 0 && firstColon !== lastColon) {
      const bracketIdx = cleanServer.lastIndexOf(']');
      if (bracketIdx > 0 && lastColon > bracketIdx) {
        portColon = lastColon;
      }
    }

    if (portColon > 0) {
      const portCandidate = cleanServer.slice(portColon + 1);
      if (/^\d+$/.test(portCandidate)) {
        port = parseInt(portCandidate, 10);
        cleanServer = cleanServer.slice(0, portColon);
      }
    }

    if (!cleanServer || !cleanShare) {
      showToast('Server and Share name are required', 'error');
      return;
    }

    setSmbTesting(true);
    setSmbTestResult(null);

    const sanitizedCfg = {
      ...shareCfg,
      server: cleanServer,
      port,
      share: cleanShare,
      path: cleanPath,
      username: (shareCfg.username || '').trim(),
      workgroup: (shareCfg.workgroup || '').trim() || 'WORKGROUP'
    };

    try {
      const data = await testSmb(sanitizedCfg);
      if (data && typeof data === 'object') {
        setSmbTestResult(data);
        if (data.success) {
          showToast(data.message || 'Connection successful', 'success');
        } else {
          showToast(data.message || data.error || 'Connection failed', 'error');
        }
      } else {
        const fallbackMsg = 'Connection test returned unexpected response';
        setSmbTestResult({ success: false, message: fallbackMsg });
        showToast(fallbackMsg, 'error');
      }
    } catch (e) {
      const errMsg = e && e.message ? e.message : String(e) || 'Unknown error';
      setSmbTestResult({ success: false, message: errMsg });
      showToast('Connection test error: ' + errMsg, 'error');
    } finally {
      setSmbTesting(false);
    }
  };

  return {
    showSmbModal,
    setShowSmbModal,
    smbEditIndex,
    setSmbEditIndex,
    smbForm,
    setSmbForm,
    smbTesting,
    smbTestResult,
    setSmbTestResult,
    handleSaveSmbShare,
    handleRemoveSmbShare,
    handleToggleSmbShare,
    handleTestSmbConnection
  };
}
