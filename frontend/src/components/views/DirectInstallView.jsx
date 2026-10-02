import React, { useEffect, useMemo, useRef, useState } from 'react';
import { formatBytes } from '../../utils/formatters';
import { getInstallStorageOptions, getTitlePlatform } from '../../utils/installStorage';
import { collectInputPkgFiles } from '../../utils/collectPkgFiles';
import {
  describeRun, groupByTitle, isDirectStorage, isQueueable, pendingBaseTitles, queuePositions, queueSpaceShortfall, totalSize,
} from '../../utils/directQueue';
import { FILTERS, groupSummary, matchesFilter, statusChip } from '../../utils/directInstallStatus';
import { Banner, dangerButton, primaryButton, toolButton } from '../directInstall/ui';
import { GameRow, PackageRow } from '../directInstall/PackageRows';
import ActiveInstallPanel from '../directInstall/ActiveInstallPanel';
import OngoingInstall from '../directInstall/OngoingInstall';

const GROUP_PREF = 'directInstallGroupByGame';
const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;
const ids = (items) => items.map((item) => item.id);

function readGroupPref() {
  try { return localStorage.getItem(GROUP_PREF) !== '0'; } catch (e) { return true; }
}

function toggleInSet(set, id) {
  const next = new Set(set);
  if (next.has(id)) next.delete(id); else next.add(id);
  return next;
}

// Highlights the page while files are dragged over it. Drops are handled app-wide.
function useDragging() {
  const [dragging, setDragging] = useState(false);
  useEffect(() => {
    const listeners = {
      dragover: (event) => { if (Array.from(event.dataTransfer?.types || []).includes('Files')) setDragging(true); },
      dragleave: (event) => { if (!event.relatedTarget) setDragging(false); },
      drop: () => setDragging(false),
    };
    Object.entries(listeners).forEach(([name, listener]) => window.addEventListener(name, listener));
    return () => Object.entries(listeners).forEach(([name, listener]) => window.removeEventListener(name, listener));
  }, []);
  return dragging;
}

// Largest free space on any drive a title can install to, by platform.
function useFreeSpace(storage) {
  return useMemo(() => {
    const cache = new Map();
    const freeFor = (titleId) => {
      const platform = getTitlePlatform(titleId);
      if (!cache.has(platform)) {
        cache.set(platform, getInstallStorageOptions(storage, titleId).reduce((max, option) => Math.max(max, option.free), 0));
      }
      return cache.get(platform);
    };
    const tooBig = (item) => Boolean(storage && item.details) && item.file.size > freeFor(item.details.title_id);
    return { freeFor, tooBig };
  }, [storage]);
}

const FolderIcon = () => (
  <svg className="w-12 h-12 text-zinc-500" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
    <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
    <path d="M12 11v5m0-5-2 2m2-2 2 2" />
  </svg>
);

export default function DirectInstallView({ up, queue, monitor, onBack, storage, installerStatus, etaInfo, debugEnabled = false }) {
  const fileRef = useRef(null);
  const folderRef = useRef(null);
  const dragging = useDragging();
  const [expanded, setExpanded] = useState(() => new Set());
  // Games the user opened or closed; others are open unless fully installed.
  const [groupOpen, setGroupOpen] = useState(() => new Map());
  const [grouped, setGrouped] = useState(readGroupPref);
  const [filter, setFilter] = useState('all');
  const [search, setSearch] = useState('');
  const { items, skipped, running, activeId, lastRun } = queue;
  const { overview } = monitor;
  const { freeFor, tooBig } = useFreeSpace(storage);

  const pendingBases = useMemo(() => pendingBaseTitles(items), [items]);
  const positions = useMemo(() => queuePositions(items, activeId), [items, activeId]);
  const chips = useMemo(() => new Map(items.map((item) =>
    [item.id, statusChip(item, pendingBases, overview.progress.label, positions.get(item.id))])),
  [items, pendingBases, overview.progress.label, positions]);
  const filterCounts = useMemo(() => Object.fromEntries(FILTERS.map(({ key }) =>
    [key, items.filter((item) => matchesFilter(key, item, chips.get(item.id))).length])), [items, chips]);
  const visible = useMemo(() => {
    const q = search.trim().toLowerCase();
    return items.filter((item) => matchesFilter(filter, item, chips.get(item.id)) && (!q ||
      [item.path, item.details?.title_name, item.details?.title_id, item.details?.content_id]
        .some((value) => value && value.toLowerCase().includes(q))));
  }, [items, chips, filter, search]);
  const groups = useMemo(() => groupByTitle(visible), [visible]);

  // Bulk actions leave out packages that cannot fit, and never touch the one installing.
  const addable = (list) => list.filter((item) => !item.queued && isQueueable(item, pendingBases) && !tooBig(item));
  const removable = (list) => list.filter((item) => item.queued && item.id !== activeId);
  const queued = items.filter((item) => item.queued);
  const addableIds = ids(addable(items));
  const removableIds = ids(removable(items));
  // Platforms short of space if `extraIds` were queued as well.
  const shortfallWith = (extraIds = []) => {
    if (!storage) return [];
    const extra = new Set(extraIds);
    return queueSpaceShortfall(items.map((item) => (extra.has(item.id) ? { ...item, queued: true } : item)),
      getTitlePlatform, freeFor);
  };
  const shortfall = shortfallWith();
  // The console cannot cancel a direct-storage install, so Skip would leave it broken.
  const canSkip = running && Boolean(activeId) && removableIds.length > 0 && !isDirectStorage(overview.progress.live);

  const choose = (event) => {
    queue.addFiles(collectInputPkgFiles(event.target.files));
    event.target.value = '';
  };
  const toggleGrouped = () => setGrouped((prev) => {
    try { localStorage.setItem(GROUP_PREF, prev ? '0' : '1'); } catch (e) {}
    return !prev;
  });
  // Asks before starting a queue (plus `extraIds` about to be queued) that does not fit.
  const confirmSpace = (extraIds) => {
    const needed = shortfallWith(extraIds);
    return !needed.length || window.confirm(`The queue needs more space than is free (${needed.map((entry) =>
      `${entry.platform}: ${formatBytes(entry.bytes)} needed, ${formatBytes(entry.free)} free`).join('; ')}). Start anyway?`);
  };

  const renderPackage = (item, nested) => {
    const oversized = tooBig(item);
    return (
      <PackageRow key={item.id} item={item} chip={chips.get(item.id)} nested={nested} active={item.id === activeId}
        open={expanded.has(item.id)} onToggle={() => setExpanded((prev) => toggleInSet(prev, item.id))}
        tooBig={oversized} canQueue={isQueueable(item, pendingBases) && !oversized} canSkip={canSkip} running={running}
        actions={{
          retry: () => queue.retry(item.id),
          setQueued: (value) => queue.setQueued(item.id, value),
          installNow: () => queue.installNow(item.id),
          remove: () => queue.remove(item.id),
          skip: monitor.skipCurrent,
        }} />
    );
  };

  const renderGame = (group) => {
    const chip = groupSummary(group, chips, pendingBases, activeId);
    const open = groupOpen.has(group.key) ? groupOpen.get(group.key) : chip.label !== 'installed';
    return (
      <React.Fragment key={group.key}>
        <GameRow group={group} chip={chip} open={open}
          onToggle={() => setGroupOpen((prev) => new Map(prev).set(group.key, !open))}
          highlighted={group.items.some((item) => item.id === activeId)}
          addableIds={ids(addable(group.items))} unqueueIds={ids(removable(group.items))}
          onQueue={queue.queueAll} onUnqueue={queue.unqueue} />
        {open && group.items.map((item) => renderPackage(item, true))}
      </React.Fragment>
    );
  };

  const addButtons = (
    <>
      <button type="button" className={toolButton} onClick={() => fileRef.current?.click()}>Add files</button>
      <button type="button" className={toolButton} onClick={() => folderRef.current?.click()}>Add folder</button>
    </>
  );
  const dropHighlight = dragging ? 'border-[#0095ff] bg-[#0095ff]/10' : '';

  return (
    <div className="max-w-6xl mx-auto space-y-4 text-zinc-200">
      <div className="flex items-center justify-between">
        <h2 className="text-xl font-bold text-white">Direct Install</h2>
        <button type="button" onClick={onBack} className={toolButton}>Back</button>
      </div>
      <p className="text-sm text-zinc-400">
        Drop .pkg files or folders anywhere on this page, or browse for them. Queued packages stream to the console one
        at a time, one game after another in the order you queued them. Keep this browser open and this computer awake until the queue finishes.
      </p>

      <input ref={fileRef} type="file" accept=".pkg" multiple className="hidden" onChange={choose} />
      <input ref={folderRef} type="file" webkitdirectory="" directory="" multiple className="hidden" onChange={choose} />

      {!items.length ? (
        <div className={'flex flex-col items-center justify-center gap-4 py-20 rounded border-2 border-dashed transition-colors ' +
          (dropHighlight || 'border-white/15 bg-white/[0.02]')}>
          <FolderIcon />
          <div className="text-center">
            <p className="font-semibold text-zinc-200">Drop .pkg files or folders here</p>
            <p className="text-xs text-zinc-500 mt-1">Folders are searched for packages, including subfolders</p>
          </div>
          <div className="flex gap-2">{addButtons}</div>
        </div>
      ) : (
        <>
          <div className="flex flex-wrap items-center gap-2">
            {addButtons}
            <div className="flex-1 min-w-[12rem] px-3 py-1.5 rounded bg-white/5 border border-white/10 text-xs text-zinc-400 truncate">
              {`${plural(items.length, 'package')} · ${formatBytes(totalSize(items))}`}
              {queued.length ? ` · ${queued.length} queued (${formatBytes(totalSize(queued))})` : ''}
            </div>
            {!addableIds.length && removableIds.length ? (
              <button type="button" className={toolButton} onClick={() => queue.unqueue(removableIds)}>− Remove all from Queue</button>
            ) : (
              <button type="button" className={toolButton} disabled={!addableIds.length}
                onClick={() => queue.queueAll(addableIds)}>+ Add all to Queue</button>
            )}
            {running ? (
              <button type="button" onClick={monitor.cancelQueue} className={dangerButton}>Stop queue</button>
            ) : queued.length ? (
              <button type="button" onClick={() => confirmSpace() && queue.start()} className={primaryButton}>
                Install queue ({queued.length})
              </button>
            ) : (
              <button type="button" onClick={() => confirmSpace(addableIds) && queue.startAll(addableIds)} disabled={!addableIds.length}
                className={primaryButton}>
                Install all{addableIds.length ? ` (${addableIds.length})` : ''}
              </button>
            )}
            <button type="button" className={toolButton} disabled={running} onClick={queue.clear}>Clear</button>
          </div>

          <div className="flex flex-wrap items-center gap-2">
            <div className="flex rounded border border-white/10 overflow-hidden text-xs" role="group" aria-label="Filter packages">
              {FILTERS.map(({ key, label }) => (
                <button key={key} type="button" onClick={() => setFilter(key)} aria-pressed={filter === key}
                  className={'px-3 py-1.5 cursor-pointer border-r border-white/10 last:border-r-0 ' +
                    (filter === key ? 'bg-[#0095ff]/20 text-white' : 'bg-white/5 text-zinc-400 hover:text-zinc-200')}>
                  {label} <span className="text-zinc-500">{filterCounts[key]}</span>
                </button>
              ))}
            </div>
            <label className="flex items-center gap-1.5 text-xs text-zinc-300 cursor-pointer select-none">
              <input type="checkbox" checked={grouped} onChange={toggleGrouped} className="accent-[#0095ff]" />
              Group by game
            </label>
            <input type="search" value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search"
              className="ml-auto w-48 px-3 py-1.5 rounded bg-white/5 border border-white/10 text-xs text-zinc-200 placeholder-zinc-500 outline-none focus:border-[#0095ff]" />
          </div>
        </>
      )}

      {lastRun && !running && (
        <Banner tone={lastRun.failed ? 'red' : lastRun.stopped ? 'amber' : 'green'} onDismiss={queue.dismissLastRun}>
          {describeRun(lastRun)}
        </Banner>
      )}
      {skipped.length > 0 && (
        <Banner onDismiss={queue.dismissSkipped}>
          <details>
            <summary className="cursor-pointer">
              Skipped {plural(skipped.length, 'unreadable file')} (not a valid PKG, or not the first part of a split package)
            </summary>
            <ul className="mt-1 space-y-0.5 text-amber-200/80">
              {skipped.map((entry) => <li key={entry.id} className="font-mono break-all">{entry.path} — {entry.error}</li>)}
            </ul>
          </details>
        </Banner>
      )}
      {shortfall.length > 0 && !running && (
        <Banner>
          Not enough free space for the whole queue: {shortfall.map((entry) =>
            `${entry.platform} packages need ${formatBytes(entry.bytes)}, ${formatBytes(entry.free)} free`).join('; ')}.
        </Banner>
      )}
      {installerStatus?.is_installing && !running && (
        <OngoingInstall status={installerStatus} etaInfo={etaInfo} onCancel={monitor.cancelOngoing} stalled={monitor.ongoingStalled} />
      )}
      {running && overview.active && (
        <ActiveInstallPanel item={overview.active} progress={overview.progress} overall={overview.overall} up={up}
          etaInfo={etaInfo} canSkip={canSkip} stalled={monitor.activeStalled} keepAwake={monitor.keepAwake}
          debugEnabled={debugEnabled} onSkip={monitor.skipCurrent} onCancel={monitor.cancelQueue} />
      )}

      {items.length > 0 && (
        <div className={'overflow-x-auto rounded border transition-colors ' + (dropHighlight || 'border-white/10 bg-white/[0.03]')}>
          <table className="w-full min-w-[760px] text-sm">
            <thead>
              <tr className="text-left text-xs text-zinc-400 border-b border-white/10">
                <th className="w-10" />
                <th className="py-3 font-medium">Name</th>
                <th className="py-3 font-medium w-28">Title ID</th>
                <th className="py-3 font-medium w-32">Type</th>
                <th className="py-3 font-medium w-32 text-center">Status</th>
                <th className="py-3 font-medium w-24 text-right">Size</th>
                <th className="py-3 font-medium w-28 text-center">Operation</th>
              </tr>
            </thead>
            <tbody>
              {!visible.length && (
                <tr><td colSpan={7} className="py-14 text-center text-zinc-500">No packages match this filter.</td></tr>
              )}
              {grouped ? groups.map(renderGame) : visible.map((item) => renderPackage(item, false))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
