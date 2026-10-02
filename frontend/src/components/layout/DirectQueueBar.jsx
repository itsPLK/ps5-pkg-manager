import React from 'react';
import { formatEta } from '../../utils/formatters';
import { ProgressBar } from '../directInstall/ui';

// Slim bar under the header while the user is elsewhere in the app: this tab's
// running queue, or else an ongoing direct install it does not own.
export default function DirectQueueBar({ overview, ongoing, stalled = false, onOpen }) {
  const { active, progress, overall } = overview;
  let title;
  let detail;
  let percent;
  if (active) {
    title = active.details?.title_name || active.file.name;
    detail = [progress.label, overall.count > 1 && `Package ${overall.position} of ${overall.count}`,
      overall.etaSeconds !== null && `about ${formatEta(overall.etaSeconds)} left`].filter(Boolean).join(' · ');
    percent = overall.count > 1 ? overall.percent : progress.percent;
  } else if (ongoing) {
    title = ongoing.title_name || 'Installing package';
    detail = ongoing.progress >= 0 ? `${ongoing.progress.toFixed(1)}%` : 'Track progress in the PS5 notifications';
    percent = Math.max(0, ongoing.progress);
  } else {
    return null;
  }
  const accent = stalled ? 'text-amber-300' : 'text-[#5cb8ff]';
  return (
    <button type="button" onClick={onOpen}
      className={'w-full text-left px-4 py-2 border-b cursor-pointer ' + (stalled
        ? 'bg-amber-500/10 border-amber-500/30 hover:bg-amber-500/15'
        : 'bg-[#0095ff]/10 border-[#0095ff]/30 hover:bg-[#0095ff]/15')}>
      <div className="flex items-center gap-3 text-xs">
        <span className={'shrink-0 font-bold uppercase tracking-wider ' + accent}>{active ? 'Direct Install' : 'Ongoing installation'}</span>
        <span className="truncate text-zinc-200">{title}</span>
        <span className="shrink-0 text-zinc-400">{stalled ? 'Stalled: no data received' : detail}</span>
        <span className={'ml-auto shrink-0 ' + accent}>Open</span>
      </div>
      <div className="mt-1.5">
        <ProgressBar percent={percent} height="h-1" color={stalled ? 'bg-amber-500/70' : 'bg-[#0070d1]'} />
      </div>
    </button>
  );
}
