import React, { useEffect, useRef, useState } from 'react';
import { formatBytes } from '../../utils/formatters';
import { getInstallStorageOptions } from '../../utils/installStorage';

export default function DirectInstallView({ up, onBack, storage, installerStatus }) {
  const fileRef = useRef(null);
  const [dragging, setDragging] = useState(false);
  const busy = up.state === 'uploading';
  const canChoose = !busy && !up.installing && up.state !== 'checking' && up.state !== 'complete' &&
    !(up.state === 'error' && up.sessionId);
  const pct = up.total > 0 ? Math.min(100, Math.round(up.offset / up.total * 100)) : 0;
  const free = storage
    ? getInstallStorageOptions(storage, up.details?.title_id)
      .reduce((max, option) => Math.max(max, option.free), 0)
    : null;
  const notEnoughSpace = free !== null && up.total > free;
  const anotherInstallActive = installerStatus?.is_installing && !up.installing && up.state !== 'uploading';

  useEffect(() => {
    const over = (event) => {
      if (!Array.from(event.dataTransfer?.types || []).includes('Files')) return;
      event.preventDefault();
      setDragging(true);
    };
    const leave = (event) => {
      if (!event.relatedTarget) setDragging(false);
    };
    const drop = (event) => {
      if (event.__pkgManagerDropHandled || !event.dataTransfer?.files?.length) return;
      event.preventDefault();
      event.__pkgManagerDropHandled = true;
      setDragging(false);
      if (canChoose) up.selectFile(event.dataTransfer.files[0]);
    };
    window.addEventListener('dragover', over);
    window.addEventListener('dragleave', leave);
    window.addEventListener('drop', drop);
    return () => {
      window.removeEventListener('dragover', over);
      window.removeEventListener('dragleave', leave);
      window.removeEventListener('drop', drop);
    };
  }, [canChoose, up.selectFile]);

  const choose = (event) => {
    const file = event.target.files?.[0];
    if (file) up.selectFile(file);
    event.target.value = '';
  };

  return (
    <div className="max-w-2xl mx-auto bg-white/5 border border-white/10 rounded p-6 space-y-5">
      <div className="flex items-center justify-between">
        <h2 className="text-xl font-bold text-white">Direct Install</h2>
        <button type="button" onClick={onBack}
          className="px-3 py-1.5 rounded bg-white/10 hover:bg-white/15 border border-white/10 text-xs font-semibold text-zinc-200 cursor-pointer">
          Back
        </button>
      </div>

      <p className="text-sm text-zinc-400">Choose a .pkg file on this computer. Review its details, then press Install to stream it to the console.</p>

      <input ref={fileRef} type="file" accept=".pkg" className="hidden" onChange={choose} disabled={!canChoose} />
      {canChoose && (
        <div className={'border-2 border-dashed rounded p-10 text-center transition-colors ' +
          (dragging ? 'border-[#0095ff] bg-[#0095ff]/10' : 'border-white/20 bg-black/10')}>
          <p className="text-zinc-200 font-semibold">Drop a .pkg file anywhere on this page</p>
          <p className="text-xs text-zinc-500 my-3">or</p>
          <button type="button" onClick={() => fileRef.current?.click()}
            className="px-5 py-2.5 rounded bg-[#0095ff] hover:bg-[#007fd6] text-white text-sm font-semibold cursor-pointer">
            Browse files
          </button>
        </div>
      )}

      {up.details && (
        <div className="flex gap-4 p-4 rounded bg-white/5 border border-white/10">
          <div className="w-20 h-20 shrink-0 rounded bg-black/30 border border-white/10 flex items-center justify-center overflow-hidden">
            {up.iconUrl ? <img src={up.iconUrl} alt="Package icon" className="w-full h-full object-cover" /> :
              <span className="text-zinc-500 text-xs">PKG</span>}
          </div>
          <div className="min-w-0 space-y-1">
            <h3 className="text-white font-bold truncate">{up.details.title_name}</h3>
            <p className="text-xs text-zinc-400 font-mono truncate">{up.details.title_id || up.details.content_id || up.fileName}</p>
            <p className="text-xs text-zinc-400">{up.details.pkg_type} · {up.details.app_version || 'Version unknown'} · {formatBytes(up.total)}</p>
            <p className="text-xs text-zinc-500 truncate">{up.fileName}</p>
          </div>
        </div>
      )}

      {up.eligibility?.can_install === false && (
        <p className="text-sm text-amber-400">{up.eligibility.install_disabled_reason}</p>
      )}
      {notEnoughSpace && <p className="text-sm text-amber-400">Not enough storage space to install this package.</p>}
      {anotherInstallActive && <p className="text-sm text-amber-400">Another package is currently installing.</p>}
      {up.state === 'checking' && <p className="text-sm text-zinc-400">Checking package installation…</p>}
      {up.state === 'selected' && up.eligibility?.can_install && (
        <button type="button" onClick={up.upload} disabled={notEnoughSpace || anotherInstallActive}
          className="w-full px-4 py-3 rounded bg-green-600 hover:bg-green-500 text-white font-semibold cursor-pointer disabled:opacity-50 disabled:cursor-not-allowed">
          Install
        </button>
      )}

      {(busy || up.state === 'complete') && (
        <div className="space-y-2">
          <div className="flex justify-between text-xs font-mono text-zinc-400">
            <span>Sending package</span><span>{formatBytes(up.offset)} / {formatBytes(up.total)} ({pct}%)</span>
          </div>
          <div className="w-full h-2.5 bg-white/10 rounded overflow-hidden">
            <div className="h-full bg-[#0095ff]" style={{ width: pct + '%' }} />
          </div>
          <p className="text-xs text-zinc-400">
            {up.installing ? 'Installation started. Keep this browser open until it finishes.' :
              busy ? 'Preparing package and waiting for the installer…' : 'Installation complete.'}
          </p>
        </div>
      )}

      {up.state === 'reading' && <p className="text-sm text-zinc-400">Reading package details…</p>}
      {up.error && <p className="text-sm text-red-400">{up.error}</p>}
      {up.state === 'error' && up.sessionId && up.details && (
        <button type="button" onClick={up.upload}
          className="px-4 py-2 rounded bg-[#0095ff] hover:bg-[#007fd6] text-white text-sm font-semibold cursor-pointer">
          Resume installation
        </button>
      )}
      {busy && <button type="button" onClick={up.cancel}
        className="px-4 py-2 rounded bg-red-600 hover:bg-red-500 text-white text-sm font-semibold cursor-pointer">Cancel upload</button>}
      {(up.state === 'error' || up.state === 'canceled' || up.state === 'complete') && (
        <button type="button" onClick={async () => { if (up.sessionId) await up.cancel(); up.reset(); }}
          className="px-4 py-2 rounded bg-white/10 hover:bg-white/15 border border-white/10 text-zinc-200 text-sm font-semibold cursor-pointer">Start over</button>
      )}
    </div>
  );
}
