import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { build } from 'esbuild';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { queueOverview } from '../src/utils/directQueue.js';

const require = createRequire(import.meta.url);
const bundle = await build({
  entryPoints: [fileURLToPath(new URL('../src/components/views/DirectInstallView.jsx', import.meta.url))],
  bundle: true, write: false, format: 'esm', platform: 'node',
  plugins: [{ name: 'shared-react', setup(builder) {
    builder.onResolve({ filter: /^react$/ }, () => ({ path: pathToFileURL(require.resolve('react')).href, external: true }));
  } }],
});
const { default: View } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`);

const item = (id, type, overrides = {}) => ({
  id, path: `CTR/${id}.pkg`, file: { name: `${id}.pkg`, size: 1024 * 1024 }, status: 'ready', queued: false,
  iconUrl: '', error: '', eligibility: { can_install: true, is_installed: false },
  details: { title_id: 'CUSA14876', title_name: type === 'base' ? 'Crash Team Racing' : `CTR ${id}`, pkg_type: type },
  ...overrides,
});
const noop = () => {};
const render = (queueProps, { speed = 0, ongoingStalled = false, activeStalled = false, ...props } = {}) => {
  const queue = { items: [], skipped: [], running: false, activeId: '', runIds: [], lastRun: null, dismissSkipped: noop,
    ...queueProps };
  const view = { up: { total: 0, offset: 0, state: 'idle', uploadSpeed: 0 }, storage: null, installerStatus: {}, ...props };
  const monitor = {
    overview: queueOverview(queue, view.installerStatus, view.up, speed),
    ongoingStalled, activeStalled, keepAwake: false, cancelQueue: noop, skipCurrent: noop, cancelOngoing: noop,
  };
  return renderToStaticMarkup(React.createElement(View, { queue, monitor, onBack: noop, ...view }));
};

test('packages are grouped under their game with a type summary', () => {
  const html = render({ items: [
    item('base', 'base'),
    item('kart', 'dlc', { status: 'blocked', eligibility: { can_install: false, is_installed: false } }),
    item('oxide', 'dlc', { status: 'blocked', eligibility: { can_install: false, is_installed: true, install_disabled_reason: 'DLC is already installed' } }),
  ] });
  assert.match(html, /Crash Team Racing/);
  assert.match(html, /Base · 2 DLC/);
  assert.match(html, /after base/);
  assert.match(html, />installed</);
  assert.match(html, /3 packages/);
});

test('unreadable files are summarized instead of listed', () => {
  const html = render({ items: [item('base', 'base')],
    skipped: [{ id: 'x', path: 'CTR/part1.pkg', error: 'Not a PKG file' }] });
  assert.match(html, /Skipped 1 unreadable file/);
  assert.match(html, /CTR\/part1\.pkg — Not a PKG file/);
});

test('the active package shows install progress inline with the queue', () => {
  const active = item('base', 'base', { status: 'installing', queued: true });
  const html = render({ items: [active, item('update', 'update', { queued: true })], running: true, activeId: 'base',
    runIds: ['base'] }, {
    up: { total: 100, offset: 100, state: 'uploading', uploadSpeed: 0 },
    installerStatus: { is_installing: true, pkg_path: 'live:abc', progress: 3.9, downloaded_bytes: 1024, total_bytes: 4096 },
    etaInfo: { text: '14m 35s remaining', speedStr: '41.98 MB/s' },
  });
  assert.match(html, /Installing to PS5/);
  assert.match(html, /Package 1 of 2/);
  assert.match(html, /installing 3\.9%/);
  assert.match(html, /14m 35s remaining/);
  assert.match(html, /Cancel queue/);
});

test('an empty list shows a drop area instead of the table', () => {
  const html = render({});
  assert.match(html, /Drop \.pkg files or folders here/);
  assert.doesNotMatch(html, /<table/);
});

test('failed packages offer a retry and show their reason in the row', () => {
  const html = render({ items: [item('base', 'base', { status: 'failed', error: 'Upload stalled' })] });
  assert.match(html, /title="Check again and queue"/);
  assert.match(html, /<p class="[^"]*truncate text-xs[^"]*" title="Upload stalled">Upload stalled<\/p>/);
});

test('with nothing queued the primary action installs everything', () => {
  const html = render({ items: [item('base', 'base'), item('update', 'update')] });
  assert.match(html, /Install all \(2\)/);
  assert.match(html, /Problems <span[^>]*>0<\/span>/);
});

test('a fully installed game starts collapsed', () => {
  const done = { status: 'done' };
  const html = render({ items: [item('base', 'base', done), item('update', 'update', done)] });
  assert.match(html, /Crash Team Racing/);
  assert.doesNotMatch(html, /base\.pkg/);
});

test('the queue summary and a space shortfall are reported', () => {
  const html = render({ items: [item('base', 'base', { queued: true })],
    lastRun: { complete: 2, failed: 1, skipped: 0, canceled: 0, stopped: false } },
  { storage: { internal: { free: 1024 } } });
  assert.match(html, /Queue finished: 2 installed · 1 failed/);
  assert.match(html, /Not enough free space for the whole queue: PS4 packages need 1 MB, 1 KB free/);
});

test('bulk actions leave out packages that cannot fit, and a busy console shows as waiting', () => {
  const html = render({ items: [
    item('base', 'base'),
    item('huge', 'update', { file: { name: 'huge.pkg', size: 4 * 1024 * 1024 } }),
    item('dlc', 'dlc', { status: 'waiting', queued: true }),
  ] }, { storage: { internal: { free: 2 * 1024 * 1024 } } });
  assert.match(html, /\+ Add all to Queue/);
  assert.match(html, /Install queue \(1\)/);
  assert.match(html, /waiting for console/);
  assert.match(html, /<p class="[^"]*truncate text-xs[^"]*" title="Not enough storage space">/);
});

test('one queue button adds while anything is left, then removes all but the installing package', () => {
  const idle = render({ items: [item('base', 'base', { queued: true }), item('update', 'update', { queued: true })] });
  assert.match(idle, /− Remove all from Queue/);
  assert.doesNotMatch(idle, /Add all to Queue/);
  const mixed = render({ items: [item('base', 'base', { queued: true }), item('update', 'update')] });
  assert.match(mixed, /\+ Add all to Queue/);
  assert.doesNotMatch(mixed, /Remove all from Queue/);
  const onlyActive = render({ items: [item('base', 'base', { queued: true, status: 'installing' })],
    running: true, activeId: 'base', runIds: ['base'] });
  assert.doesNotMatch(onlyActive, /Remove all from Queue/);
  assert.doesNotMatch(render({ items: [item('base', 'base')] }), /Remove all from Queue/);
});

test('Skip is offered only when another package is queued and the install can be canceled', () => {
  const run = (installerStatus, next = true) => render({
    items: [item('base', 'base', { queued: true, status: 'installing' }), item('update', 'update', { queued: next })],
    running: true, activeId: 'base', runIds: ['base'],
  }, { up: { total: 100, offset: 50, state: 'uploading', uploadSpeed: 0 }, installerStatus });
  const installing = { is_installing: true, pkg_path: 'live:abc', progress: 10, downloaded_bytes: 1, total_bytes: 10 };
  assert.match(run(installing), />Skip</);
  assert.match(run(installing), /title="Skip this package"/);
  assert.match(run({}), />Skip</);
  assert.doesNotMatch(run({ ...installing, is_direct_storage: true }), />Skip</);
  assert.doesNotMatch(run({ ...installing, progress: -1 }), />Skip</);
  assert.doesNotMatch(run(installing, false), />Skip</);
});

test('an install this page does not own shows as an ongoing installation', () => {
  const status = { is_installing: true, pkg_path: 'live:abc', title_name: 'EA SPORTS FC 27', title_id: 'CUSA57220',
    progress: 20.6, downloaded_bytes: 2048, total_bytes: 8192 };
  const html = render({}, { installerStatus: status, etaInfo: { text: '12m 39s remaining' } });
  assert.match(html, /Ongoing installation/);
  assert.match(html, /EA SPORTS FC 27/);
  assert.match(html, /20\.6%/);
  assert.match(html, /12m 39s remaining/);
  assert.match(html, /src="\/api\/icon\?path=live%3Aabc"/);
  assert.match(html, />Cancel</);
  const directStorage = render({}, { installerStatus: { ...status, progress: -1 } });
  assert.match(directStorage, /track progress in the PS5 notifications/);
  assert.doesNotMatch(directStorage, />Cancel</);
});

test('a stalled ongoing installation says why and how to clear it', () => {
  const status = { is_installing: true, pkg_path: 'live:abc', title_name: 'EA SPORTS FC 27', progress: 47.6,
    downloaded_bytes: 4, total_bytes: 8 };
  const html = render({}, { installerStatus: status, etaInfo: { text: 'Calculating ETA...' }, ongoingStalled: true });
  assert.match(html, /Stalled: the console has received no data/);
  assert.doesNotMatch(html, /Calculating ETA/);
  assert.match(html, />Cancel</);
});

test('a stalled queued package explains what to do', () => {
  const active = item('base', 'base', { status: 'installing', queued: true });
  const html = render({ items: [active, item('update', 'update', { queued: true })], running: true, activeId: 'base',
    runIds: ['base'] }, {
    up: { total: 100, offset: 100, state: 'uploading', uploadSpeed: 0 },
    installerStatus: { is_installing: true, pkg_path: 'live:abc', progress: 10.9, downloaded_bytes: 1, total_bytes: 10 },
    activeStalled: true,
  });
  assert.match(html, /The PS5 is not reading this package\. If there is still no progress in 30 seconds/);
  assert.match(html, /To move on now, press Skip/);
});

test('a stalled package hides its frozen ETA and the growing queue estimate', () => {
  const active = item('base', 'base', { status: 'installing', queued: true });
  const html = render({ items: [active, item('update', 'update', { queued: true })], running: true, activeId: 'base',
    runIds: ['base'] }, {
    up: { total: 100, offset: 100, state: 'uploading', uploadSpeed: 0 },
    installerStatus: { is_installing: true, pkg_path: 'live:abc', progress: 2.1, downloaded_bytes: 1, total_bytes: 10 },
    etaInfo: { text: '15m 5s remaining', speedStr: '46.01 MB/s' },
    speed: 300 * 1024,
    activeStalled: true,
  });
  assert.match(html, /No progress/);
  assert.doesNotMatch(html, /15m 5s remaining/);
  assert.doesNotMatch(html, /for the whole queue/);
});
