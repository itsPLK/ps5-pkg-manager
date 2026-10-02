// How a Direct Install package or game is labeled and filtered in the list.
import { isQueueable } from './directQueue.js';

const INSTALLED_REASON = /already installed|same or newer/i;

export const TYPE_LABEL = { base: 'Base', update: 'Update', dlc: 'DLC' };

export const FILTERS = [
  { key: 'all', label: 'All' },
  { key: 'ready', label: 'Ready' },
  { key: 'queued', label: 'Queued' },
  { key: 'installed', label: 'Installed' },
  { key: 'problems', label: 'Problems' },
];

// { label, tone, reason } for one package. `activeLabel` describes the package
// installing now; `position` is its place in the queue.
export function statusChip(item, pendingBases, activeLabel, position) {
  const reason = item.error || item.eligibility?.install_disabled_reason || '';
  const queued = { label: position ? `queued · ${position}` : 'queued', tone: 'blue' };
  switch (item.status) {
    case 'reading': return { label: 'reading', tone: 'muted' };
    case 'checking': return { label: 'checking', tone: 'muted' };
    case 'waiting': return { label: 'waiting for console', tone: 'muted', reason: 'Another install is using the console' };
    case 'installing': return { label: activeLabel, tone: 'blue' };
    case 'done': return { label: 'installed', tone: 'green' };
    case 'failed': return { label: 'failed', tone: 'red', reason };
    case 'error': return { label: 'check failed', tone: 'red', reason };
    case 'blocked':
      if (INSTALLED_REASON.test(reason)) return { label: 'installed', tone: 'green', reason };
      if (isQueueable(item, pendingBases)) {
        return { ...(item.queued ? queued : { label: 'after base', tone: 'blue' }), reason: 'Installs after its base package' };
      }
      return { label: 'blocked', tone: 'amber', reason };
    default:
      return item.queued ? queued : { label: 'ready', tone: 'muted', reason: item.error };
  }
}

export function matchesFilter(filter, item, chip) {
  switch (filter) {
    case 'ready': return !item.queued && (chip.label === 'ready' || chip.label === 'after base');
    case 'queued': return item.queued || item.status === 'installing' || item.status === 'waiting';
    case 'installed': return chip.label === 'installed';
    case 'problems': return chip.tone === 'red' || chip.tone === 'amber';
    default: return true;
  }
}

// One chip summarizing a game from its packages' chips.
export function groupSummary(group, chips, pendingBases, activeId) {
  const active = group.items.find((item) => item.id === activeId);
  if (active) return chips.get(active.id);
  const count = (test) => group.items.filter(test).length;
  const queued = count((item) => item.queued);
  if (queued) return { label: `${queued} queued`, tone: 'blue' };
  if (count((item) => chips.get(item.id).label === 'installed') === group.items.length) return { label: 'installed', tone: 'green' };
  const ready = count((item) => isQueueable(item, pendingBases));
  if (ready) return { label: `${ready} ready`, tone: 'muted' };
  if (count((item) => item.status === 'reading' || item.status === 'checking')) return { label: 'checking', tone: 'muted' };
  if (count((item) => chips.get(item.id).tone === 'red')) return { label: 'failed', tone: 'red' };
  return { label: 'blocked', tone: 'amber' };
}

// "Base · Update · 3 DLC"
export function typeCounts(items) {
  const counts = new Map();
  items.forEach((item) => {
    const type = TYPE_LABEL[item.details?.pkg_type];
    if (type) counts.set(type, (counts.get(type) || 0) + 1);
  });
  return [...counts].map(([type, n]) => (n > 1 ? `${n} ${type}` : type)).join(' · ');
}
