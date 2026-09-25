import React, { useState } from 'react';
import { SmbSharePickerModal, SmbFolderPickerModal } from './SmbBrowseModals';
import { browseSmb } from '../../api/smb';

export default function SmbShareModal({
  show, onClose, isEditing, form, setForm, testResult, testing, onTest, onSave
}) {
  const [sharePickerOpen, setSharePickerOpen] = useState(false);
  const [folderPickerOpen, setFolderPickerOpen] = useState(false);
  const [openingFolder, setOpeningFolder] = useState(false);
  const [folderBrowseError, setFolderBrowseError] = useState('');
  const [folderPreflight, setFolderPreflight] = useState(null);

  if (!show) return null;

  const isServerValid = form && form.server && form.server.trim();
  const isShareValid = form && form.share && form.share.trim();
  const isSaveDisabled = !isServerValid || !isShareValid;

  const portStr = form.port && parseInt(form.port, 10) !== 445 ? `:${parseInt(form.port, 10)}` : '';
  const cleanPath = (form.path || '').replace(/^[\\/]+|[\\/]+$/g, '').replace(/\\/g, '/');
  const selectedUrl = isShareValid
    ? `smb://${(form.server || '').trim()}${portStr}/${form.share.trim()}${cleanPath ? `/${cleanPath}` : ''}`
    : '';

  const connection = {
    id: form.id || '',
    server: form.server || '',
    port: form.port || 445,
    username: form.username || '',
    password: form.password || '',
    workgroup: form.workgroup || 'WORKGROUP'
  };

  const openFolderPicker = async () => {
    if (!isShareValid || openingFolder) return;

    setOpeningFolder(true);
    setFolderBrowseError('');
    try {
      const data = await browseSmb({ ...connection, share: form.share.trim(), path: cleanPath });
      if (!data || !data.success || !Array.isArray(data.entries)) {
        setFolderPreflight(null);
        setFolderBrowseError((data && (data.error || data.message)) || 'Could not access this share. Check the server and password.');
        return;
      }

      setFolderPreflight({ path: cleanPath, entries: data.entries, next: data.next_cursor });
      setFolderPickerOpen(true);
    } catch (e) {
      setFolderPreflight(null);
      setFolderBrowseError(e && e.message ? e.message : 'Could not access this share. Check the server and password.');
    } finally {
      setOpeningFolder(false);
    }
  };

  return (
    <div data-modal-dialog="true" role="dialog" aria-modal="true" className="fixed inset-0 z-50 bg-black/90 flex items-center justify-center p-4">
      <div className="bg-[#181a27] border border-white/15 rounded-[2px] max-w-lg w-full p-6 space-y-5 overflow-y-auto max-h-[90vh]">
        <div className="flex items-center justify-between pb-3 border-b border-white/10">
          <div className="flex items-center space-x-3">
            <div className="w-10 h-10 rounded-[2px] bg-cyan-600/20 border border-cyan-500/30 flex items-center justify-center text-cyan-400 shrink-0">
              <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                <rect x="2" y="2" width="20" height="8" rx="2" />
                <rect x="2" y="14" width="20" height="8" rx="2" />
                <line x1="6" y1="6" x2="6.01" y2="6" />
                <line x1="6" y1="18" x2="6.01" y2="18" />
                <path d="M12 10v4" />
              </svg>
            </div>
            <div>
              <h3 className="text-lg font-bold text-white">
                {isEditing ? 'Edit Samba Share' : 'Add Samba Share'}
              </h3>
              <p className="text-xs text-zinc-400">Enter connection details, then pick a shared folder</p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="text-zinc-500 hover:text-white text-lg font-bold px-2 py-1 cursor-pointer transition-colors"
          >
            &times;
          </button>
        </div>

        <div className="space-y-4 text-xs">
          {/* Share Label */}
          <div>
            <label className="block font-semibold text-zinc-300 mb-1">Display Label (Optional)</label>
            <input
              type="text"
              placeholder="e.g. NAS Games, My PC"
              value={form.label || ''}
              onChange={(e) => setForm({ ...form, label: e.target.value })}
              className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-white/40"
            />
          </div>

          {/* Server & Port */}
          <div className="grid grid-cols-3 gap-3">
            <div className="col-span-2">
              <label className="block font-semibold text-zinc-300 mb-1">Server IP or Hostname *</label>
              <input
                type="text"
                placeholder="192.168.1.100 or nas.local"
                value={form.server || ''}
                onChange={(e) => setForm({ ...form, server: e.target.value })}
                className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 font-mono focus:outline-none focus:border-white/40"
              />
            </div>
            <div>
              <label className="block font-semibold text-zinc-300 mb-1">Port</label>
              <input
                type="number"
                placeholder="445"
                value={typeof form.port !== 'undefined' && form.port !== null ? form.port : ''}
                onChange={(e) => setForm({ ...form, port: e.target.value === '' ? '' : (parseInt(e.target.value, 10) || '') })}
                className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white font-mono focus:outline-none focus:border-white/40"
              />
            </div>
          </div>

          {/* Credentials */}
          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block font-semibold text-zinc-300 mb-1">Username (Guest if blank)</label>
              <input
                type="text"
                placeholder="anonymous"
                value={form.username || ''}
                onChange={(e) => setForm({ ...form, username: e.target.value })}
                className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-white/40"
              />
            </div>
            <div>
              <label className="block font-semibold text-zinc-300 mb-1">Password</label>
              <input
                type="password"
                placeholder="••••••••"
                value={form.password || ''}
                onChange={(e) => setForm({ ...form, password: e.target.value })}
                className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-white/40"
              />
            </div>
          </div>

          {/* Workgroup */}
          <div>
            <label className="block font-semibold text-zinc-300 mb-1">Workgroup / Domain</label>
            <input
              type="text"
              placeholder="WORKGROUP"
              value={form.workgroup || ''}
              onChange={(e) => setForm({ ...form, workgroup: e.target.value })}
              className="w-full bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm text-white placeholder-zinc-500 focus:outline-none focus:border-white/40"
            />
          </div>

          {/* Share selector */}
          <div>
            <label className="block font-semibold text-zinc-300 mb-1">Share *</label>
            <div className="flex gap-2">
              <input
                aria-label="Share name"
                type="text"
                placeholder="e.g. shared"
                value={form.share || ''}
                onChange={(e) => setForm({ ...form, share: e.target.value, path: '' })}
                className="flex-1 min-w-0 bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm font-mono text-white placeholder-zinc-500 focus:outline-none focus:border-white/40"
              />
              <button
                type="button"
                onClick={() => setSharePickerOpen(true)}
                disabled={!isServerValid}
                className="px-4 py-2.5 rounded-[2px] bg-cyan-600 hover:bg-cyan-500 text-white text-xs font-bold transition-colors disabled:opacity-40 cursor-pointer shrink-0"
              >
                Select share
              </button>
            </div>
            <p className="text-[11px] text-zinc-500 mt-1">Enter the share name directly if the server does not allow listing shares.</p>
            {!isServerValid && (
              <p className="text-[11px] text-zinc-500 mt-1">Enter a server above first.</p>
            )}
          </div>

          {/* Folder selector (only once a share is picked) */}
          {isShareValid && (
            <div>
              <label className="block font-semibold text-zinc-300 mb-1">Folder (Optional)</label>
              <div className="flex gap-2">
                <div className="flex-1 min-w-0 bg-black/50 border border-white/15 rounded-[2px] px-3.5 py-2.5 text-sm font-mono truncate">
                  {cleanPath
                    ? <span className="text-white">{cleanPath}</span>
                    : <span className="text-zinc-500">Share root</span>}
                </div>
                <button
                  type="button"
                  onClick={openFolderPicker}
                  disabled={openingFolder}
                  className="px-4 py-2.5 rounded-[2px] bg-white/10 hover:bg-white/15 text-zinc-200 text-xs font-semibold transition-colors disabled:opacity-40 cursor-pointer shrink-0"
                >
                  {openingFolder ? 'Checking...' : 'Browse...'}
                </button>
                {cleanPath && (
                  <button
                    type="button"
                    onClick={() => setForm({ ...form, path: '' })}
                    className="px-3 py-2.5 rounded-[2px] bg-white/5 hover:bg-white/10 text-zinc-400 hover:text-white text-xs font-semibold transition-colors cursor-pointer shrink-0"
                  >
                    Clear
                  </button>
                )}
              </div>
              {folderBrowseError && (
                <p className="text-[11px] text-rose-300 mt-1">{folderBrowseError}</p>
              )}
            </div>
          )}

          {/* Selected path preview */}
          {selectedUrl ? (
            <p className="text-xs font-mono text-cyan-300/90 break-all select-all">{selectedUrl}</p>
          ) : (
            <p className="text-[11px] text-zinc-500">No folder selected yet.</p>
          )}

          {/* Browse Only Mode */}
          <div className="pt-2 border-t border-white/10">
            <label className="flex items-start space-x-3 text-zinc-300 cursor-pointer select-none">
              <input
                type="checkbox"
                className="ps5-focus-item mt-0.5 rounded-[2px]"
                checked={!!form.browse_only}
                onChange={(event) => setForm({ ...form, browse_only: event.target.checked })}
              />
              <div>
                <span className="font-semibold text-white block text-xs">Browse only</span>
                <span className="text-[11px] text-zinc-400 leading-relaxed block">
                  Skip full and background scans. Select individual PKGs to install directly from the file browser (recommended for large shares).
                </span>
              </div>
            </label>
          </div>

          {/* Test Result Banner */}
          {testResult && (
            <div className={`rounded-[2px] p-3 border text-xs flex items-center space-x-2.5 ${
              testResult.success
                ? 'bg-emerald-500/15 border-emerald-500/30 text-emerald-300'
                : 'bg-rose-500/15 border-rose-500/30 text-rose-300'
            }`}>
              <span className="font-bold">{testResult.success ? '✓' : '✗'}</span>
              <span className="flex-1">{testResult.message || testResult.error || (testResult.success ? 'Connected successfully!' : 'Connection failed')}</span>
            </div>
          )}
        </div>

        {/* Buttons */}
        <div className="flex items-center justify-between pt-2 border-t border-white/10">
          <button
            type="button"
            onClick={onTest}
            disabled={testing || isSaveDisabled}
            className="px-4 py-2.5 rounded-[2px] ps5-focus-item bg-white/10 hover:bg-white/15 text-zinc-200 text-xs font-semibold transition-colors disabled:opacity-40 cursor-pointer flex items-center space-x-2"
          >
            {testing && <div className="ps5-robust-spinner-sm" />}
            <span>{testing ? 'Testing...' : 'Test Connection'}</span>
          </button>

          <div className="flex items-center space-x-3">
            <button
              type="button"
              onClick={onClose}
              className="px-4 py-2.5 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 text-zinc-400 hover:text-white text-xs font-semibold transition-colors cursor-pointer"
            >
              Cancel
            </button>
            <button
              type="button"
              onClick={onSave}
              disabled={isSaveDisabled}
              className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-cyan-600 hover:bg-cyan-500 text-white text-xs font-bold transition-colors disabled:opacity-40 cursor-pointer"
            >
              Save Share
            </button>
          </div>
        </div>
      </div>

      <SmbSharePickerModal
        show={sharePickerOpen}
        onClose={() => setSharePickerOpen(false)}
        connection={connection}
        selectedShare={form.share || ''}
        onSelect={(name) => setForm({ ...form, share: name, path: '' })}
      />

      <SmbFolderPickerModal
        show={folderPickerOpen}
        onClose={() => setFolderPickerOpen(false)}
        connection={connection}
        share={form.share || ''}
        initialPath={form.path || ''}
        initialEntries={folderPreflight && folderPreflight.entries}
        initialEntriesPath={folderPreflight && folderPreflight.path}
        initialNextCursor={folderPreflight && folderPreflight.next}
        onSelect={(path) => setForm({ ...form, path })}
      />
    </div>
  );
}
