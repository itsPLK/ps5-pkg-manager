import React from 'react';
import { iconUrlFor } from '../../BlurIcon';
import { formatBytes } from '../../utils/formatters';
import { isDirectStorage } from '../../utils/directQueue';
import { Icon, InstallHeading, ProgressBar, panelButton } from './ui';

// An install this page's queue does not own: started from another window, the
// library, or an upload whose queue was lost (for example after a reload).
export default function OngoingInstall({ status, etaInfo, onCancel, stalled }) {
  const directStorage = isDirectStorage(status);
  const percent = Math.min(100, Math.max(0, status.progress || 0));
  return (
    <div className={'p-4 rounded border space-y-3 ' + (stalled ? 'border-amber-500/40 bg-amber-500/[0.06]' : 'border-white/15 bg-white/[0.04]')}>
      <div className="flex gap-4">
        <Icon url={iconUrlFor(status.pkg_path)} size="w-16 h-16" />
        <div className="flex-1 min-w-0 space-y-2">
          <div className="flex items-start justify-between gap-3">
            <InstallHeading kicker="Ongoing installation" kickerClass="text-zinc-400"
              title={status.title_name || 'Installing package'} ids={[status.title_id, status.content_id]} />
            {!directStorage && onCancel && (
              <button type="button" onClick={onCancel} className={panelButton + ' shrink-0 hover:bg-red-600/80'}>Cancel</button>
            )}
          </div>
          {directStorage ? (
            <p className="text-xs text-zinc-400">Direct storage install: track progress in the PS5 notifications.</p>
          ) : (
            <>
              <ProgressBar percent={percent} height="h-2" />
              <div className="flex flex-wrap justify-between gap-x-4 text-xs text-zinc-300">
                <span>
                  <span className="font-bold text-white">{percent.toFixed(1)}%</span>
                  <span className="text-zinc-400 ml-2">({formatBytes(status.downloaded_bytes)} / {formatBytes(status.total_bytes)})</span>
                </span>
                {!stalled && etaInfo?.text && <span className="text-zinc-400">{etaInfo.text}</span>}
              </div>
            </>
          )}
        </div>
      </div>
      {stalled ? (
        <p className="text-xs text-amber-300">
          Stalled: the console has received no data for over a minute. The browser tab that was sending this package
          was probably closed or reloaded, so it cannot continue. Cancel it, then install the package again.
        </p>
      ) : (
        <p className="text-xs text-zinc-500">A queue started now waits until this installation finishes.</p>
      )}
    </div>
  );
}
