import React from 'react';
import { formatBytes } from '../../utils/formatters';
import { isRetryable } from '../../utils/directQueue';
import { TYPE_LABEL, typeCounts } from '../../utils/directInstallStatus';
import { Chevron, Chip, Icon, roundButton, stopPropagation } from './ui';

const REASON_TONE = { red: 'text-red-300/90', amber: 'text-amber-300/90', blue: 'text-[#5cb8ff]/80' };

const SkipIcon = () => (
  <svg className="w-3.5 h-3.5" viewBox="0 0 24 24" fill="currentColor"><path d="M5 5v14l9-7zm10 0h3v14h-3z" /></svg>
);
const InstallIcon = () => (
  <svg className="w-3.5 h-3.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
    <path d="M12 3v12m0 0-4-4m4 4 4-4M5 21h14" />
  </svg>
);

// `actions`: { retry, setQueued, installNow, remove, skip } for this package.
export function PackageRow({ item, chip, nested, active, open, onToggle, tooBig, canQueue, canSkip, running, actions }) {
  const d = item.details;
  const reason = tooBig ? 'Not enough storage space' : chip.reason;
  const reasonTone = tooBig ? REASON_TONE.red : REASON_TONE[chip.tone] || 'text-zinc-500';
  const detail = (label, value, mono) => (
    <p><span className="text-zinc-500">{label}:</span> <span className={mono ? 'font-mono break-all' : ''}>{value}</span></p>
  );
  return (
    <>
      <tr className={'border-b border-white/5 ' + (active ? 'bg-[#0095ff]/10' : 'hover:bg-white/[0.04]')}>
        <td className="text-center"><Chevron open={open} onClick={onToggle} label="Show details" /></td>
        <td className={'py-2.5 pr-3 ' + (nested ? 'pl-6' : '')}>
          <div className="flex items-center gap-3 min-w-0">
            <Icon url={item.iconUrl} size={nested ? 'w-7 h-7' : 'w-9 h-9'} />
            <div className="min-w-0">
              <p className="truncate text-zinc-100" title={item.path}>{item.file.name}</p>
              {reason && chip.label !== 'installed' ? (
                <p className={'truncate text-xs ' + reasonTone} title={reason}>{reason}</p>
              ) : d?.title_name && <p className="truncate text-xs text-zinc-500">{d.title_name}</p>}
            </div>
          </div>
        </td>
        <td className="py-2.5 font-mono text-xs text-zinc-300">{d?.title_id || ''}</td>
        <td className="py-2.5 text-xs text-zinc-400">{TYPE_LABEL[d?.pkg_type] || ''}</td>
        <td className="py-2.5 text-center"><Chip tone={chip.tone} title={chip.reason}>{chip.label}</Chip></td>
        <td className="py-2.5 text-right"><Chip tone={tooBig ? 'red' : 'muted'}>{formatBytes(item.file.size)}</Chip></td>
        <td className="py-2.5">
          <div className="flex items-center justify-center gap-2">
            {isRetryable(item) ? (
              <button type="button" className={roundButton} title="Check again and queue" disabled={active}
                onClick={actions.retry}>↻</button>
            ) : active && canSkip ? (
              <button type="button" className={roundButton} title="Skip this package" onClick={actions.skip}><SkipIcon /></button>
            ) : item.queued ? (
              <button type="button" className={roundButton} title="Remove from queue" disabled={active}
                onClick={() => actions.setQueued(false)}>−</button>
            ) : (
              <button type="button" className={roundButton} title="Add to queue" disabled={!canQueue}
                onClick={() => actions.setQueued(true)}>+</button>
            )}
            <button type="button" className={roundButton} title="Install now"
              disabled={running || item.status !== 'ready' || tooBig} onClick={actions.installNow}><InstallIcon /></button>
          </div>
        </td>
      </tr>
      {open && (
        <tr className="border-b border-white/5 bg-black/20">
          <td />
          <td colSpan={6} className={'py-3 pr-4 text-xs text-zinc-400 space-y-1 ' + (nested ? 'pl-6' : '')}>
            {detail('File', item.path, true)}
            {d && detail('Title', `${d.title_name || 'Unknown'} · ${d.app_version || 'Version unknown'}`)}
            {d?.content_id && detail('Content ID', d.content_id, true)}
            {item.eligibility?.installed_version && detail('Installed version', item.eligibility.installed_version)}
            {chip.reason && <p className={chip.tone === 'red' ? 'text-red-300' : 'text-amber-300'}>{chip.reason}</p>}
            {tooBig && <p className="text-amber-300">Not enough storage space to install this package.</p>}
            {!item.queued && !active && (
              <button type="button" onClick={actions.remove}
                className="mt-1 text-zinc-400 hover:text-red-300 underline cursor-pointer">Remove from list</button>
            )}
          </td>
        </tr>
      )}
    </>
  );
}

// Header row of a game; its + / − button adds or removes the whole game.
export function GameRow({ group, chip, open, onToggle, highlighted, addableIds, unqueueIds, onQueue, onUnqueue }) {
  const count = group.items.length;
  return (
    <tr onClick={onToggle}
      className={'border-b border-white/10 cursor-pointer ' + (highlighted ? 'bg-[#0095ff]/10' : 'bg-white/[0.04] hover:bg-white/[0.06]')}>
      <td className="text-center"><Chevron open={open} onClick={stopPropagation(onToggle)} label="Show packages" /></td>
      <td className="py-2.5 pr-3">
        <div className="flex items-center gap-3 min-w-0">
          <Icon url={group.iconUrl} size="w-10 h-10" />
          <div className="min-w-0">
            <p className="truncate font-semibold text-white">{group.title}</p>
            <p className="truncate text-xs text-zinc-500">{count} package{count === 1 ? '' : 's'}</p>
          </div>
        </div>
      </td>
      <td className="py-2.5 font-mono text-xs text-zinc-300">{group.titleId}</td>
      <td className="py-2.5 text-xs text-zinc-400">{typeCounts(group.items)}</td>
      <td className="py-2.5 text-center"><Chip tone={chip.tone}>{chip.label}</Chip></td>
      <td className="py-2.5 text-right"><Chip tone="blue">{formatBytes(group.size)}</Chip></td>
      <td className="py-2.5">
        <div className="flex items-center justify-center gap-2">
          {!addableIds.length && unqueueIds.length ? (
            <button type="button" className={roundButton} title="Remove game from queue"
              onClick={stopPropagation(() => onUnqueue(unqueueIds))}>−</button>
          ) : (
            <button type="button" className={roundButton} title="Add game to queue" disabled={!addableIds.length}
              onClick={stopPropagation(() => onQueue(addableIds))}>+</button>
          )}
          <span className="w-8" />
        </div>
      </td>
    </tr>
  );
}
