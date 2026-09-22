import React, { useCallback, useEffect, useState } from 'react';
import { listSmbShares, browseSmb } from '../../api/smb';

function splitPath(path) {
  return (path || '').split('/').filter(Boolean);
}

function FolderIcon({ className }) {
  return (
    <svg className={className || 'w-4 h-4'} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
      <path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z" />
    </svg>
  );
}

function pickerShell(children, onClose, title, subtitle) {
  return (
    <div data-modal-dialog="true" role="dialog" aria-modal="true" className="fixed inset-0 z-[60] bg-black/90 flex items-center justify-center p-4">
      <div className="bg-[#181a27] border border-white/15 rounded-[2px] max-w-md w-full p-6 space-y-4 overflow-y-auto max-h-[90vh]">
        <div className="flex items-center justify-between pb-3 border-b border-white/10">
          <div>
            <h3 className="text-lg font-bold text-white">{title}</h3>
            {subtitle && <p className="text-xs text-zinc-400 font-mono break-all">{subtitle}</p>}
          </div>
          <button
            type="button"
            onClick={onClose}
            className="text-zinc-500 hover:text-white text-lg font-bold px-2 py-1 cursor-pointer transition-colors"
          >
            &times;
          </button>
        </div>
        {children}
      </div>
    </div>
  );
}

export function SmbSharePickerModal({ show, onClose, connection, selectedShare, onSelect }) {
  const [shares, setShares] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    setLoading(true);
    setError('');
    try {
      const data = await listSmbShares(connection || {});
      if (data && data.success && Array.isArray(data.shares)) {
        setShares(data.shares);
        if (!data.shares.length) setError('No shares found on this server.');
      } else {
        setShares([]);
        setError((data && (data.error || data.message)) || 'Could not list shares.');
      }
    } catch (e) {
      setShares([]);
      setError(e && e.message ? e.message : 'Could not list shares.');
    } finally {
      setLoading(false);
    }
  }, [connection]);

  useEffect(() => {
    if (show) load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [show]);

  if (!show) return null;

  const visible = shares.filter((s) => s && s.name && !s.is_special);
  const hiddenCount = shares.length - visible.length;

  const body = (
    <>
      {loading ? (
        <p className="text-xs text-zinc-400 flex items-center space-x-2">
          <span className="ps5-robust-spinner-sm" />
          <span>Searching for shares...</span>
        </p>
      ) : error && !visible.length ? (
        <div className="space-y-3">
          <p className="text-xs text-rose-300">{error}</p>
          <button
            type="button"
            onClick={load}
            className="px-3 py-1.5 rounded-[2px] bg-white/10 hover:bg-white/15 text-zinc-200 text-xs font-semibold cursor-pointer transition-colors"
          >
            Try again
          </button>
        </div>
      ) : (
        <div className="max-h-72 overflow-y-auto rounded-[2px] border border-white/10 divide-y divide-white/5">
          {visible.map((s) => (
            <button
              key={s.name}
              type="button"
              onClick={() => { onSelect(s.name); onClose(); }}
              className={`w-full text-left px-3 py-2.5 flex items-center space-x-3 cursor-pointer transition-colors ${selectedShare === s.name ? 'bg-cyan-600/20' : 'bg-white/[0.02] hover:bg-white/[0.07]'}`}
            >
              <span className="text-cyan-400 shrink-0"><FolderIcon /></span>
              <span className="min-w-0 flex-1">
                <span className="block text-sm font-semibold text-white font-mono truncate">{s.name}</span>
                {s.remark && <span className="block text-[11px] text-zinc-500 truncate">{s.remark}</span>}
              </span>
              {s.is_hidden && (
                <span className="text-[10px] px-1.5 py-0.5 rounded-[2px] bg-white/5 border border-white/10 text-zinc-400 shrink-0">hidden</span>
              )}
              {selectedShare === s.name && <span className="text-cyan-300 text-xs font-bold shrink-0">✓</span>}
            </button>
          ))}
          {hiddenCount > 0 && (
            <p className="px-3 py-2 text-[11px] text-zinc-500">{hiddenCount} system share{hiddenCount === 1 ? '' : 's'} hidden</p>
          )}
        </div>
      )}
      <div className="flex items-center justify-between pt-2 border-t border-white/10">
        <button
          type="button"
          onClick={load}
          disabled={loading}
          className="px-3 py-2 rounded-[2px] bg-white/5 hover:bg-white/10 text-zinc-200 text-xs font-semibold cursor-pointer transition-colors disabled:opacity-40"
        >
          Refresh
        </button>
        <button
          type="button"
          onClick={onClose}
          className="px-4 py-2 rounded-[2px] bg-white/5 hover:bg-white/10 text-zinc-400 hover:text-white text-xs font-semibold cursor-pointer transition-colors"
        >
          Cancel
        </button>
      </div>
    </>
  );

  return pickerShell(body, onClose, 'Select Share', (connection && connection.server) || '');
}

export function SmbFolderPickerModal({ show, onClose, connection, share, initialPath, onSelect }) {
  const [segs, setSegs] = useState([]);
  const [entries, setEntries] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  const load = useCallback(async (pathSegs) => {
    const path = (pathSegs || []).join('/');
    setLoading(true);
    setError('');
    try {
      const data = await browseSmb({ ...(connection || {}), share, path });
      if (data && data.success && Array.isArray(data.entries)) {
        setEntries(data.entries);
      } else {
        setEntries([]);
        setError((data && (data.error || data.message)) || 'Could not list this folder.');
      }
    } catch (e) {
      setEntries([]);
      setError(e && e.message ? e.message : 'Could not list this folder.');
    } finally {
      setLoading(false);
    }
  }, [connection, share]);

  useEffect(() => {
    if (show) {
      const start = splitPath(initialPath || '');
      setSegs(start);
      setEntries([]);
      load(start);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [show]);

  if (!show) return null;

  const currentPath = segs.join('/');
  const subtitle = `smb://${(connection && connection.server) || ''}/${share || ''}${currentPath ? `/${currentPath}` : ''}`;
  const dirs = entries.filter((e) => e.is_dir);
  const pkgs = entries.filter((e) => !e.is_dir);

  const go = (nextSegs) => {
    setSegs(nextSegs);
    load(nextSegs);
  };

  const body = (
    <>
      <div className="flex items-center flex-wrap gap-1 text-[11px]">
        <button
          type="button"
          onClick={() => go([])}
          className="px-2 py-1 rounded-[2px] border bg-white/5 border-white/10 text-zinc-200 hover:bg-white/10 font-mono cursor-pointer transition-colors"
        >
          {share || 'Share root'}
        </button>
        {segs.map((seg, i) => (
          <React.Fragment key={i}>
            <span className="text-zinc-600">/</span>
            <button
              type="button"
              onClick={() => go(segs.slice(0, i + 1))}
              className="px-2 py-1 rounded-[2px] border bg-white/5 border-white/10 text-zinc-300 hover:bg-white/10 font-mono cursor-pointer transition-colors"
            >
              {seg}
            </button>
          </React.Fragment>
        ))}
      </div>

      {loading ? (
        <p className="text-xs text-zinc-400 flex items-center space-x-2">
          <span className="ps5-robust-spinner-sm" />
          <span>Loading folders...</span>
        </p>
      ) : error ? (
        <div className="space-y-3">
          <p className="text-xs text-rose-300">{error}</p>
          <button
            type="button"
            onClick={() => load(segs)}
            className="px-3 py-1.5 rounded-[2px] bg-white/10 hover:bg-white/15 text-zinc-200 text-xs font-semibold cursor-pointer transition-colors"
          >
            Try again
          </button>
        </div>
      ) : (
        <div className="max-h-72 overflow-y-auto rounded-[2px] border border-white/10 divide-y divide-white/5">
          {dirs.length === 0 && entries.length === 0 && (
            <p className="px-3 py-3 text-[11px] text-zinc-500">No subfolders here. PKGs directly in this folder will still be found when scanning.</p>
          )}
          {dirs.map((e) => (
            <button
              key={e.name}
              type="button"
              onClick={() => go([...segs, e.name])}
              className="w-full text-left px-3 py-2.5 flex items-center space-x-3 bg-white/[0.02] hover:bg-white/[0.07] cursor-pointer transition-colors"
            >
              <span className="text-cyan-400 shrink-0"><FolderIcon /></span>
              <span className="min-w-0 flex-1 block text-sm text-white font-mono truncate">{e.name}</span>
              <span className="text-zinc-500 shrink-0">›</span>
            </button>
          ))}
          {pkgs.length > 0 && (
            <>
              <p className="px-3 pt-2 text-[10px] uppercase tracking-wide text-zinc-500">Packages here</p>
              {pkgs.map((e) => (
                <div key={e.name} className="px-3 py-2 flex items-center space-x-3 bg-white/[0.01]">
                  <span className="text-emerald-400/80 shrink-0 text-xs font-bold">.pkg</span>
                  <span className="min-w-0 flex-1 block text-xs text-zinc-300 font-mono truncate">{e.name}</span>
                </div>
              ))}
            </>
          )}
        </div>
      )}

      <div className="flex items-center justify-between pt-2 border-t border-white/10">
        <button
          type="button"
          onClick={onClose}
          className="px-4 py-2 rounded-[2px] bg-white/5 hover:bg-white/10 text-zinc-400 hover:text-white text-xs font-semibold cursor-pointer transition-colors"
        >
          Cancel
        </button>
        <button
          type="button"
          onClick={() => { onSelect(currentPath); onClose(); }}
          disabled={loading}
          className="px-5 py-2 rounded-[2px] bg-cyan-600 hover:bg-cyan-500 text-white text-xs font-bold cursor-pointer transition-colors disabled:opacity-40"
        >
          Use this folder
        </button>
      </div>
    </>
  );

  return pickerShell(body, onClose, 'Select Folder', subtitle);
}
