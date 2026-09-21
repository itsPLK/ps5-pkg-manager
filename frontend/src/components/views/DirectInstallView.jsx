import React, { useRef, useState, useEffect } from 'react';
import { useDirectUpload } from '../../hooks/useDirectUpload';
import { installPackage, pollStatus } from '../../api/installer';
import { formatBytes } from '../../utils/formatters';

export default function DirectInstallView({ onBack, showToast }) {
  const up = useDirectUpload();
  const fileRef = useRef(null);
  const [installing, setInstalling] = useState(false);
  const [installed, setInstalled] = useState(0);
  const [installTotal, setInstallTotal] = useState(0);
  const [installDone, setInstallDone] = useState('');

  const busy = up.state === 'uploading';
  const pct = up.total > 0 ? Math.min(100, Math.round((up.offset / up.total) * 100)) : up.progress;

  useEffect(function () {
    if (!installing) return;
    let stop = false;
    const timer = setInterval(async function () {
      try {
        const st = await pollStatus();
        if (stop) return;
        setInstalled(st.downloaded_bytes || 0);
        setInstallTotal(st.total_bytes || up.total);
        if (!st.is_installing) {
          clearInterval(timer);
          setInstalling(false);
          if (st.completed) {
            setInstallDone('Installed. Ready to play.');
            if (showToast) showToast('Installation complete', 'success');
          } else if (st.failed) {
            setInstallDone('Install failed. Re-upload to retry.');
            if (showToast) showToast('Installation failed', 'error');
          }
        }
      } catch (e) {}
    }, 1000);
    return function () {
      stop = true;
      clearInterval(timer);
    };
  }, [installing, up.total, showToast]);

  const pick = function (e) {
    const f = e.target.files && e.target.files[0];
    if (f) {
      setInstalling(false);
      setInstallDone('');
      up.upload(f);
    }
  };

  const doInstall = async function () {
    if (!up.installPath || installing) return;
    setInstalling(true);
    setInstallDone('');
    setInstalled(0);
    setInstallTotal(up.total);
    try {
      await installPackage(up.installPath);
      if (showToast) showToast('Installation started', 'success');
    } catch (e) {
      setInstalling(false);
      if (showToast) showToast('Install failed: ' + e.message, 'error');
    }
  };

  const isBusyError = up.state === 'error' && /active|busy|409/i.test(up.error);

  return (
    <div className="max-w-2xl mx-auto bg-white/5 border border-white/10 rounded p-6 space-y-5">
      <div className="flex items-center justify-between">
        <h2 className="text-xl font-bold text-white">Direct Install</h2>
        <button
          type="button"
          onClick={onBack}
          className="px-3 py-1.5 rounded bg-white/10 hover:bg-white/15 border border-white/10 text-xs font-semibold text-zinc-200 cursor-pointer"
        >
          Back
        </button>
      </div>

      <p className="text-sm text-zinc-400">
        Pick a <span className="text-zinc-200 font-mono">.pkg</span> on this device. It streams
        straight into the installer over WebSocket — nothing is stored on the
        console first. You can press Install as soon as the header is parsed,
        while the rest still uploads.
      </p>

      {(up.total > 70 * 1024 * 1024) && !installing && (
        <p className="text-xs text-yellow-300/90 border border-yellow-500/30 bg-yellow-500/10 rounded px-3 py-2">
          Large file: press Install early. The upload pauses once the
          console&apos;s 64&nbsp;MB window is full and resumes as the
          installation consumes data.
        </p>
      )}

      <input
        ref={fileRef}
        type="file"
        accept=".pkg,application/octet-stream"
        className="hidden"
        onChange={pick}
        disabled={busy}
      />

      {up.state === 'idle' && (
        <button
          type="button"
          onClick={function () { if (fileRef.current) fileRef.current.click(); }}
          className="w-full px-4 py-3 rounded bg-[#0095ff] hover:bg-[#007fd6] text-white font-semibold text-sm cursor-pointer"
        >
          Choose .pkg file
        </button>
      )}

      {(busy || up.state === 'complete' || up.state === 'error' || up.state === 'canceled') && (
        <div className="space-y-3">
          <div className="flex items-center justify-between text-xs font-mono text-zinc-400">
            <span className="truncate mr-3">{up.fileName}</span>
            <span>sent {formatBytes(up.offset)} / {formatBytes(up.total)} ({pct}%)</span>
          </div>
          <div className="w-full bg-white/10 h-2.5 rounded-full overflow-hidden">
            <div
              className={'h-full rounded-full transition-all duration-300 ' + (up.state === 'error' ? 'bg-red-500' : 'bg-[#0095ff]')}
              style={{ width: pct + '%' }}
            />
          </div>
          {installing && (
            <div>
              <div className="flex items-center justify-between text-xs font-mono text-zinc-400 mb-1">
                <span>installed</span>
                <span>{formatBytes(installed)} / {formatBytes(installTotal)}</span>
              </div>
              <div className="w-full bg-white/10 h-2.5 rounded-full overflow-hidden">
                <div
                  className="h-full rounded-full bg-green-500 transition-all duration-300"
                  style={{ width: (installTotal > 0 ? Math.min(100, Math.round((installed / installTotal) * 100)) : 0) + '%' }}
                />
              </div>
            </div>
          )}
          <div className="text-xs text-zinc-400">
            {up.state === 'uploading' && !up.headerReady && 'Uploading header…'}
            {up.state === 'uploading' && up.headerReady && 'Uploading… do not close this page.'}
            {up.state === 'complete' && (
              <span className="text-green-400">Upload complete. Session stays live for the installer.</span>
            )}
            {up.state === 'error' && <span className="text-red-400">Error: {up.error}</span>}
            {up.state === 'canceled' && 'Upload canceled.'}
            {installDone !== '' && <div className="mt-1">{installDone}</div>}
          </div>
        </div>
      )}

      <div className="flex items-center space-x-3">
        {busy && (
          <button
            type="button"
            onClick={up.cancel}
            className="px-4 py-2 rounded bg-red-600 hover:bg-red-500 text-white text-sm font-semibold cursor-pointer"
          >
            Cancel
          </button>
        )}
        {(up.headerReady || up.state === 'complete') && !installing && installDone === '' && (
          <button
            type="button"
            onClick={doInstall}
            className="px-4 py-2 rounded bg-green-600 hover:bg-green-500 text-white text-sm font-semibold cursor-pointer"
          >
            {up.state === 'complete' ? 'Install now' : 'Install now (upload continues)'}
          </button>
        )}
        {isBusyError && (
          <button
            type="button"
            onClick={async function () { await up.cancel(); up.reset(); }}
            className="px-4 py-2 rounded bg-yellow-600 hover:bg-yellow-500 text-white text-sm font-semibold cursor-pointer"
          >
            Clear stuck session & retry
          </button>
        )}
        {(up.state === 'error' || up.state === 'canceled' || up.state === 'complete') && (
          <button
            type="button"
            onClick={function () { setInstalling(false); setInstallDone(''); up.reset(); }}
            className="px-4 py-2 rounded bg-white/10 hover:bg-white/15 border border-white/10 text-zinc-200 text-sm font-semibold cursor-pointer"
          >
            Start over
          </button>
        )}
      </div>

      {up.sessionId !== '' && (
        <div className="text-[11px] font-mono text-zinc-500 break-all">
          live:{up.sessionId}
        </div>
      )}
    </div>
  );
}
