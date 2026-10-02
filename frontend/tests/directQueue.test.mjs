import assert from 'node:assert/strict';
import test from 'node:test';
import {
  describeRun, groupByTitle, isQueueable, isRetryable, itemKey, liveProgress, nextQueued, pendingBaseTitles,
  queuePositions, queueSpaceShortfall, READING_GROUP, runProgress, runQueue,
} from '../src/utils/directQueue.js';

const file = (name, size = 1024) => ({ name, size, lastModified: 1 });
const pkg = (id, type, overrides = {}) => ({
  id, file: file(`${id}.pkg`), status: 'ready', queued: false, iconUrl: '',
  details: { title_id: 'CUSA00001', title_name: `Title ${id}`, pkg_type: type },
  eligibility: { can_install: true, is_installed: false },
  ...overrides,
});

test('item keys identify a file whichever folder it was picked through', () => {
  const viaParent = itemKey({ file: file('game.pkg'), path: 'ABC/B/game.pkg' });
  assert.equal(viaParent, itemKey({ file: file('game.pkg'), path: 'B/game.pkg' }));
  // A different file with the same name in another folder is still its own package.
  assert.notEqual(viaParent, itemKey({ file: file('game.pkg', 2048), path: 'C/game.pkg' }));
});

test('an update or DLC waiting on a listed base is queueable; otherwise blocked stays blocked', () => {
  const base = pkg('base', 'base');
  const dlc = pkg('dlc', 'dlc', { status: 'blocked', eligibility: { can_install: false, is_installed: false } });
  assert.equal(isQueueable(dlc, pendingBaseTitles([base, dlc])), true);
  assert.equal(isQueueable(dlc, pendingBaseTitles([dlc])), false);
  // A finished or failed base no longer unlocks anything.
  assert.equal(isQueueable(dlc, pendingBaseTitles([{ ...base, status: 'failed' }, dlc])), false);
  // Blocked for another reason while the base is already installed.
  const installed = { ...dlc, eligibility: { can_install: false, is_installed: true } };
  assert.equal(isQueueable(installed, pendingBaseTitles([base, installed])), false);
  assert.equal(isQueueable({ ...base, status: 'blocked' }, pendingBaseTitles([base])), false);
});

test('next queued item follows base, update, DLC order and skips attempted items', () => {
  const items = [pkg('dlc', 'dlc', { queued: true }), pkg('update', 'update', { queued: true }),
    pkg('base', 'base', { queued: true }), pkg('other', 'base')];
  const attempted = new Set();
  const order = [];
  for (let next; (next = nextQueued(items, attempted)); attempted.add(next.id)) order.push(next.id);
  assert.deepEqual(order, ['base', 'update', 'dlc']);
  assert.equal(nextQueued(items, new Set(), 'other').id, 'other');
});

test('runQueue installs each queued item once and honors items queued mid-run', async () => {
  let items = [pkg('base', 'base', { queued: true }), pkg('dlc', 'dlc')];
  const installed = [];
  const results = await runQueue({
    getItems: () => items,
    shouldStop: () => false,
    install: async (item) => {
      installed.push(item.id);
      // Stays queued after a failure, as a stale render would leave it.
      if (item.id === 'base') items = items.map((other) => ({ ...other, queued: true }));
      return { outcome: item.id === 'base' ? 'failed' : 'complete' };
    },
  });
  assert.deepEqual(installed, ['base', 'dlc']);
  assert.deepEqual(results.map((result) => result.outcome), ['failed', 'complete']);
});

test('runQueue stops before the next item once stopped', async () => {
  const items = [pkg('a', 'base', { queued: true }), pkg('b', 'update', { queued: true })];
  let stopped = false;
  const installed = [];
  await runQueue({
    getItems: () => items,
    shouldStop: () => stopped,
    install: async (item) => { installed.push(item.id); stopped = true; return { outcome: 'failed' }; },
  });
  assert.deepEqual(installed, ['a']);
});

test('runQueue continues a run without retrying what it already attempted', async () => {
  const items = [pkg('a', 'base', { queued: true }), pkg('b', 'update', { queued: true })];
  const installed = [];
  await runQueue({
    getItems: () => items,
    shouldStop: () => false,
    attempted: new Set(['a']),
    install: async (item) => { installed.push(item.id); return { outcome: 'complete' }; },
  });
  assert.deepEqual(installed, ['b']);
});

test('groups are named by the base package and sorted with reading files last', () => {
  const groups = groupByTitle([
    pkg('dlc', 'dlc', { iconUrl: 'blob:dlc', details: { title_id: 'CUSA2', title_name: 'Kart Pack', pkg_type: 'dlc' } }),
    { id: 'raw', file: file('raw.pkg', 5), status: 'reading', details: null },
    pkg('base', 'base', { iconUrl: 'blob:base', details: { title_id: 'CUSA2', title_name: 'Racing Game', pkg_type: 'base' } }),
    pkg('solo', 'dlc', { details: { title_id: 'CUSA1', title_name: 'Alpha DLC', pkg_type: 'dlc' } }),
  ]);
  assert.deepEqual(groups.map((group) => group.title), ['Alpha DLC', 'Racing Game', 'Reading packages…']);
  const racing = groups[1];
  assert.deepEqual(racing.items.map((item) => item.id), ['base', 'dlc']);
  assert.equal(racing.iconUrl, 'blob:base');
  assert.equal(racing.size, 2048);
  assert.equal(groups[2].key, READING_GROUP);
});

const game = (id, titleId, type, queuedAt, size = 100) => pkg(id, type, { queued: true, queuedAt,
  file: file(`${id}.pkg`, size), details: { title_id: titleId, title_name: titleId, pkg_type: type } });
const installOrder = (items) => {
  const attempted = new Set();
  const order = [];
  for (let next; (next = nextQueued(items, attempted)); attempted.add(next.id)) order.push(next.id);
  return order;
};

test('games install first in, first out, one game at a time, regardless of size', () => {
  const items = [game('smallBase', 'SMALL', 'base', 3, 1), game('bigBase', 'BIG', 'base', 1, 900),
    game('bigDlc', 'BIG', 'dlc', 2), game('smallUpdate', 'SMALL', 'update', 4, 1)];
  assert.deepEqual(installOrder(items), ['bigBase', 'bigDlc', 'smallBase', 'smallUpdate']);
});

test('a base queued after its DLC still installs first, and a later game waits its turn', () => {
  const items = [game('dlc', 'GAME', 'dlc', 1), game('other', 'OTHER', 'base', 2), game('base', 'GAME', 'base', 3)];
  assert.deepEqual(installOrder(items), ['base', 'dlc', 'other']);
});

test('queue positions follow install order and skip the package installing now', () => {
  const items = [game('b', 'B', 'base', 2), game('a', 'A', 'base', 1), game('a2', 'A', 'update', 3),
    pkg('idle', 'base')];
  assert.deepEqual([...queuePositions(items)], [['a', 1], ['a2', 2], ['b', 3]]);
  assert.deepEqual([...queuePositions(items, 'a')], [['a2', 1], ['b', 2]]);
});

test('only failed or unchecked packages are retryable', () => {
  assert.equal(isRetryable({ status: 'failed' }), true);
  assert.equal(isRetryable({ status: 'error' }), true);
  assert.equal(isRetryable({ status: 'blocked' }), false);
});

test('queue space is compared per platform against the largest free drive', () => {
  const items = [pkg('ps4', 'base', { queued: true, file: file('a', 600) }),
    pkg('ps5', 'base', { queued: true, file: file('b', 300), details: { title_id: 'PPSA00001', pkg_type: 'base' } }),
    pkg('idle', 'base', { file: file('c', 5000) })];
  const platformOf = (id) => (id.startsWith('CUSA') ? 'PS4' : 'PS5');
  const shortfall = queueSpaceShortfall(items, platformOf, (id) => (platformOf(id) === 'PS4' ? 500 : 1000));
  assert.deepEqual(shortfall, [{ platform: 'PS4', bytes: 600, free: 500 }]);
});

test('run progress counts finished packages, the active one and what is still queued', () => {
  const items = [pkg('done', 'base', { file: file('a', 100), status: 'done' }),
    pkg('active', 'update', { file: file('b', 200), queued: true, status: 'installing' }),
    pkg('next', 'dlc', { file: file('c', 100), queued: true }), pkg('other', 'dlc', { file: file('d', 999) })];
  const run = runProgress(items, ['done', 'active'], 'active', 50, 10);
  assert.deepEqual({ ...run, percent: Math.round(run.percent) },
    { position: 2, count: 3, doneBytes: 150, totalBytes: 400, percent: 38, etaSeconds: null });
  assert.equal(runProgress(items, ['done', 'active'], 'active', 50, 2048).etaSeconds, 250 / 2048);
});

test('live progress prefers the console install percentage over bytes sent', () => {
  const up = { total: 200, offset: 50, state: 'uploading' };
  assert.equal(liveProgress({}, up).label, 'sending 25%');
  const installing = { is_installing: true, pkg_path: 'live:1', progress: 12.34 };
  assert.equal(liveProgress(installing, up).label, 'installing 12.3%');
  assert.equal(liveProgress({ ...installing, progress: -1 }, up).label, 'installing');
});

test('run summaries read naturally', () => {
  assert.equal(describeRun({ complete: 3, failed: 1, skipped: 0, canceled: 0 }), 'Queue finished: 3 installed · 1 failed');
  assert.equal(describeRun({ complete: 0, failed: 0, skipped: 0, canceled: 1, stopped: true }), 'Queue stopped: 1 canceled');
});
