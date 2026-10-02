import assert from 'node:assert/strict';
import test from 'node:test';
import { groupSummary, matchesFilter, statusChip, typeCounts } from '../src/utils/directInstallStatus.js';
import { isDirectStorage, isLiveInstall, settleOutcome } from '../src/utils/directQueue.js';

const pkg = (type, overrides = {}) => ({ id: type, status: 'ready', queued: false,
  details: { title_id: 'CUSA00001', pkg_type: type }, eligibility: { is_installed: false }, ...overrides });

test('queued packages show their position, and a blocked DLC waits for a listed base', () => {
  const pendingBases = new Set(['CUSA00001']);
  assert.equal(statusChip(pkg('base', { queued: true }), pendingBases, '', 2).label, 'queued · 2');
  const dlc = pkg('dlc', { status: 'blocked' });
  assert.equal(statusChip(dlc, pendingBases, '').label, 'after base');
  assert.equal(statusChip(dlc, new Set(), '').label, 'blocked');
  const installed = pkg('update', { status: 'blocked', eligibility: { install_disabled_reason: 'Installed version is same or newer' } });
  assert.equal(statusChip(installed, pendingBases, '').label, 'installed');
});

test('filters and game summaries follow the package chips', () => {
  const failed = pkg('base', { status: 'failed', error: 'x' });
  const chip = statusChip(failed, new Set(), '');
  assert.equal(matchesFilter('problems', failed, chip), true);
  assert.equal(matchesFilter('ready', failed, chip), false);
  const group = { items: [failed, pkg('dlc', { status: 'done' })] };
  const chips = new Map(group.items.map((item) => [item.id, statusChip(item, new Set(), '')]));
  assert.deepEqual(groupSummary(group, chips, new Set(), ''), { label: 'failed', tone: 'red' });
  assert.equal(typeCounts([pkg('base'), pkg('dlc'), pkg('dlc')]), 'Base · 2 DLC');
});

test('an interrupted attempt settles by how it was interrupted', () => {
  const failed = { outcome: 'failed', error: 'Upload failed' };
  assert.deepEqual(settleOutcome({ outcome: 'complete', error: '' }, { stopped: true, skip: null }), { outcome: 'complete', error: '' });
  assert.deepEqual(settleOutcome(failed, { stopped: false, skip: { reason: 'Stalled', game: true } }), { outcome: 'failed', error: 'Stalled' });
  assert.deepEqual(settleOutcome(failed, { stopped: false, skip: { reason: '' } }), { outcome: 'canceled', error: 'Skipped' });
  assert.deepEqual(settleOutcome(failed, { stopped: true, skip: null }), { outcome: 'canceled', error: 'Canceled' });
  assert.deepEqual(settleOutcome(failed, { stopped: false, skip: null }), failed);
});

test('install state helpers recognize direct and direct-storage installs', () => {
  assert.equal(isLiveInstall({ is_installing: true, pkg_path: 'live:1' }), true);
  assert.equal(isLiveInstall({ is_installing: false, pkg_path: 'live:1' }), false);
  assert.equal(isLiveInstall({ is_installing: true, pkg_path: '/mnt/usb0/a.pkg' }), false);
  assert.equal(isDirectStorage({ progress: -1 }), true);
  assert.equal(isDirectStorage({ progress: 5, is_direct_storage: true }), true);
  assert.equal(isDirectStorage({ progress: 5 }), false);
  assert.equal(isDirectStorage(null), false);
});
