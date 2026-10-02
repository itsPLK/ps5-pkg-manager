// Direct-install queue rules, kept free of React for testing.

const TYPE_RANK = { base: 0, update: 1, dlc: 2 };
const LAST = Number.MAX_SAFE_INTEGER;

export const typeRank = (item) => TYPE_RANK[item.details?.pkg_type] ?? 3;
// Identifies a file by name, size and modified time, not its folder, so the same
// file picked again through a parent or child folder is not listed twice.
export const itemKey = ({ file }) => [file.name, file.size, file.lastModified].join('|');
export const totalSize = (items) => items.reduce((sum, item) => sum + item.file.size, 0);
export const isRetryable = (item) => item.status === 'failed' || item.status === 'error';

export const isLiveInstall = (status) => Boolean(status?.is_installing && status.pkg_path?.startsWith('live:'));
// Direct-storage installs report no byte progress and cannot be canceled by the console.
export const isDirectStorage = (status) => Boolean(status && (status.is_direct_storage || status.progress < 0));

// Title IDs whose base package is listed and not yet done.
export function pendingBaseTitles(items) {
  return new Set(items
    .filter((item) => item.details?.pkg_type === 'base' && ['ready', 'waiting', 'installing'].includes(item.status))
    .map((item) => item.details.title_id));
}

// An update or DLC may be queued with its base; the console checks it again right before its turn.
export function isQueueable(item, pendingBases) {
  if (item.status === 'ready') return true;
  const d = item.details;
  return item.status === 'blocked' && Boolean(d) && d.pkg_type !== 'base' &&
    !item.eligibility?.is_installed && pendingBases.has(d.title_id);
}

const gameKey = (item) => item.details?.title_id || item.id;
const queuedAt = (item) => item.queuedAt ?? LAST;

// First in, first out by game, one game at a time; within a game base, update, DLC.
// A game keeps its place from its first queued package, including ones already
// attempted, so a started game is never interrupted.
export function nextQueued(items, attempted, onlyId) {
  const gameQueuedAt = new Map();
  items.filter((item) => item.queued || attempted.has(item.id)).forEach((item) => {
    gameQueuedAt.set(gameKey(item), Math.min(gameQueuedAt.get(gameKey(item)) ?? LAST, queuedAt(item)));
  });
  const order = (a, b) => gameQueuedAt.get(gameKey(a)) - gameQueuedAt.get(gameKey(b)) ||
    gameKey(a).localeCompare(gameKey(b)) || typeRank(a) - typeRank(b) || queuedAt(a) - queuedAt(b) ||
    a.file.name.localeCompare(b.file.name);
  return items
    .filter((item) => !attempted.has(item.id) && (onlyId ? item.id === onlyId : item.queued))
    .reduce((best, item) => (!best || order(item, best) < 0 ? item : best), null);
}

// 1-based install position of every queued package, not counting `skipId`.
export function queuePositions(items, skipId) {
  const positions = new Map();
  const attempted = new Set(skipId ? [skipId] : []);
  for (let next; (next = nextQueued(items, attempted)); attempted.add(next.id)) positions.set(next.id, positions.size + 1);
  return positions;
}

// Installs queued items one at a time, trying each once. getItems is read before
// every pick so items queued or unqueued mid-run are honored. `attempted` may be
// shared with an earlier pass of the same run.
export async function runQueue({ getItems, install, shouldStop, onlyId, attempted = new Set() }) {
  const results = [];
  for (let next; !shouldStop() && (next = nextQueued(getItems(), attempted, onlyId));) {
    attempted.add(next.id);
    results.push({ id: next.id, ...(await install(next)) });
  }
  return results;
}

// Final outcome of an attempt given how it was interrupted (`skip`: { reason, game }).
// A finished install counts even if Stop or Skip came late.
export function settleOutcome(result, { stopped, skip }) {
  if (result.outcome === 'complete') return result;
  if (skip?.reason) return { outcome: 'failed', error: skip.reason };
  if (stopped || skip || result.outcome === 'canceled') return { outcome: 'canceled', error: stopped ? 'Canceled' : 'Skipped' };
  return result;
}

// Platforms whose queued packages need more than their largest free drive.
export function queueSpaceShortfall(items, platformOf, freeFor) {
  const need = new Map();
  items.filter((item) => item.queued && item.details).forEach((item) => {
    const titleId = item.details.title_id;
    const platform = platformOf(titleId);
    const entry = need.get(platform) || { platform, bytes: 0, free: freeFor(titleId) };
    entry.bytes += item.file.size;
    need.set(platform, entry);
  });
  return [...need.values()].filter((entry) => entry.bytes > entry.free);
}

// Console-reported progress of the active direct install, or bytes sent until
// the console starts installing.
export function liveProgress(installerStatus, up) {
  const live = isLiveInstall(installerStatus) ? installerStatus : null;
  const sentPct = up.total > 0 ? Math.min(100, Math.round(up.offset / up.total * 100)) : 0;
  const installPct = live && !isDirectStorage(live) ? Math.min(100, Math.max(0, live.progress)) : null;
  const label = installPct !== null ? `installing ${installPct.toFixed(1)}%`
    : live ? 'installing' : up.state === 'checking' ? 'checking' : `sending ${sentPct}%`;
  return { live, sentPct, installPct, label, percent: installPct ?? sentPct };
}

// Progress of the current (or last) run: packages attempted in it plus those still queued.
export function runProgress(items, runIds, activeId, activeBytes, speed) {
  const inRun = new Set(runIds);
  const members = items.filter((item) => inRun.has(item.id) || item.queued);
  const total = totalSize(members);
  const done = members.reduce((sum, item) => sum + (item.id === activeId
    ? Math.min(item.file.size, activeBytes || 0) : inRun.has(item.id) ? item.file.size : 0), 0);
  return {
    position: Math.min(runIds.length, members.length),
    count: members.length,
    doneBytes: done,
    totalBytes: total,
    percent: total > 0 ? done / total * 100 : 0,
    etaSeconds: speed >= 1024 ? Math.max(0, total - done) / speed : null,
  };
}

// What the page and the header bar show about the queue.
export function queueOverview(queue, installerStatus, up, speed) {
  const progress = liveProgress(installerStatus, up);
  const active = queue.items.find((item) => item.id === queue.activeId) || null;
  const activeBytes = !active ? 0 : progress.installPct !== null ? active.file.size * progress.installPct / 100 : up.offset;
  return { active, progress, overall: runProgress(queue.items, queue.runIds, queue.activeId, activeBytes, speed) };
}

export const READING_GROUP = '__reading';

// One group per title ID. The base (else the update) names the game and supplies
// its icon, since a DLC's own title is the add-on's name.
export function groupByTitle(items) {
  const byTitle = new Map();
  items.forEach((item) => {
    const key = item.details ? item.details.title_id || item.details.content_id || item.id : READING_GROUP;
    byTitle.set(key, [...(byTitle.get(key) || []), item]);
  });
  return Array.from(byTitle, ([key, list]) => {
    list.sort((a, b) => typeRank(a) - typeRank(b) || a.file.name.localeCompare(b.file.name));
    const lead = list.find((item) => item.details?.pkg_type === 'base') ||
      list.find((item) => item.details?.pkg_type === 'update') || list[0];
    const reading = key === READING_GROUP;
    return {
      key,
      items: list,
      title: reading ? 'Reading packages…' : lead.details?.title_name || lead.file.name,
      titleId: reading ? '' : lead.details?.title_id || '',
      iconUrl: lead.iconUrl || list.find((item) => item.iconUrl)?.iconUrl || '',
      size: totalSize(list),
    };
  }).sort((a, b) => (a.key === READING_GROUP) - (b.key === READING_GROUP) || a.title.localeCompare(b.title));
}

const RUN_OUTCOMES = [['complete', 'installed'], ['failed', 'failed'], ['skipped', 'skipped'], ['canceled', 'canceled']];

export function summarizeRun(results) {
  const counts = Object.fromEntries(RUN_OUTCOMES.map(([outcome]) => [outcome, 0]));
  results.forEach((result) => { counts[result.outcome] = (counts[result.outcome] || 0) + 1; });
  return counts;
}

export function describeRun(run) {
  const parts = RUN_OUTCOMES.filter(([outcome]) => run[outcome]).map(([outcome, label]) => `${run[outcome]} ${label}`);
  return `Queue ${run.stopped ? 'stopped' : 'finished'}: ${parts.join(' · ') || 'nothing installed'}`;
}
