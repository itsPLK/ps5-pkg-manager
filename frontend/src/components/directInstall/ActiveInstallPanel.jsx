import React from 'react';
import { formatBytes, formatEta } from '../../utils/formatters';
import { Icon, InstallHeading, ProgressBar, panelButton } from './ui';

// Progress of the package this tab's queue is installing, plus the whole run.
export default function ActiveInstallPanel({ item, progress, overall, up, etaInfo, canSkip, stalled, keepAwake,
  debugEnabled, onSkip, onCancel }) {
  const { live, installPct, sentPct } = progress;
  const kicker = !live ? 'Preparing package' : installPct === null ? 'Direct storage install' : 'Installing to PS5';
  return (
    <div className="p-4 rounded border border-[#0095ff]/40 bg-[#0095ff]/[0.06] space-y-3">
      <div className="flex gap-4">
        <Icon url={item.iconUrl} size="w-20 h-20" />
        <div className="flex-1 min-w-0 space-y-2">
          <div className="flex items-start justify-between gap-3">
            <InstallHeading kicker={kicker} kickerClass="text-[#5cb8ff]"
              title={item.details?.title_name || item.file.name} ids={[item.details?.title_id, item.details?.content_id]} />
            <div className="flex shrink-0 gap-2">
              {canSkip && (
                <button type="button" onClick={onSkip} title="Cancel this package and continue with the next"
                  className={panelButton + ' hover:bg-white/20'}>Skip</button>
              )}
              <button type="button" onClick={onCancel} className={panelButton + ' hover:bg-red-600/80'}>
                Cancel{overall.count > 1 ? ' queue' : ''}
              </button>
            </div>
          </div>
          {live?.prompt_message && <p className="text-xs text-[#5cb8ff] truncate">{live.prompt_message}</p>}
          <ProgressBar percent={progress.percent} />
          <div className="flex flex-wrap justify-between gap-x-4 text-xs text-zinc-300">
            {installPct !== null ? (
              <span>
                <span className="font-bold text-white">{installPct.toFixed(1)}%</span>
                <span className="text-zinc-400 ml-2">({formatBytes(live.downloaded_bytes)} / {formatBytes(live.total_bytes)})</span>
              </span>
            ) : (
              <span className="text-zinc-400">
                {live ? 'Track progress in the PS5 notifications. ' : ''}
                Sent {formatBytes(up.offset)} / {formatBytes(up.total)} ({sentPct}%)
              </span>
            )}
            {stalled ? (
              <span className="font-medium text-amber-300">No progress</span>
            ) : installPct !== null && etaInfo?.text && (
              <span>
                <span className="font-medium text-white">{etaInfo.text}</span>
                {etaInfo.speedStr && <span className="text-zinc-400 font-mono ml-2">({etaInfo.speedStr})</span>}
              </span>
            )}
          </div>
          {debugEnabled && up.state === 'uploading' && (
            <div className="text-right text-xs font-mono text-cyan-300">WebSocket receive: {formatBytes(up.uploadSpeed)}/s</div>
          )}
        </div>
      </div>
      {overall.count > 1 && (
        <div className="pt-3 border-t border-white/10 space-y-1.5">
          <div className="flex flex-wrap justify-between gap-x-4 text-xs text-zinc-300">
            <span>
              <span className="font-semibold text-white">Package {overall.position} of {overall.count}</span>
              <span className="text-zinc-400 ml-2">({formatBytes(overall.doneBytes)} / {formatBytes(overall.totalBytes)})</span>
            </span>
            {!stalled && overall.etaSeconds !== null && (
              <span className="text-zinc-400">About {formatEta(overall.etaSeconds)} for the whole queue</span>
            )}
          </div>
          <ProgressBar percent={overall.percent} height="h-1.5" color="bg-[#5cb8ff]/70" />
        </div>
      )}
      {stalled && (
        <p className="text-xs text-amber-300">
          The PS5 is not reading this package. If there is still no progress in 30 seconds, the queue gives
          this game up and moves on to the next one. To move on now, press {canSkip ? 'Skip' : 'Cancel'}.
        </p>
      )}
      {!keepAwake && (
        <p className="text-xs text-amber-300/90">
          Keep this computer awake: if it sleeps, the transfer stops and the current package must start over.
        </p>
      )}
    </div>
  );
}
