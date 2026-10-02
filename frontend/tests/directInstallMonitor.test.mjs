import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { build } from 'esbuild';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

const require = createRequire(import.meta.url);
const bundle = await build({
  entryPoints: [fileURLToPath(new URL('../src/hooks/useDirectInstallMonitor.js', import.meta.url))],
  bundle: true, write: false, format: 'esm', platform: 'node',
  define: { __APP_VERSION__: '"test"', __APP_COMMIT__: '"test"', __APP_BUILD_DATE__: '"test"' },
  plugins: [{ name: 'shared-react', setup(builder) {
    builder.onResolve({ filter: /^react$/ }, () => ({ path: pathToFileURL(require.resolve('react')).href, external: true }));
  } }],
});
const { useDirectInstallMonitor } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`);

const noop = () => {};
function monitorFor(queue, installerStatus, { upload = {}, cancelInstall = noop } = {}) {
  let result;
  const Probe = () => {
    result = useDirectInstallMonitor({ queue: { items: [], runIds: [], activeId: '', running: false, lastRun: null, ...queue },
      upload: { total: 0, offset: 0, state: 'idle', sessionId: '', cancel: noop, ...upload }, installerStatus, speed: 0,
      appVersion: '1.0', cancelInstall, showToast: noop });
    return null;
  };
  renderToStaticMarkup(React.createElement(Probe));
  return result;
}

test('a direct install this tab does not run is reported as ongoing', () => {
  const live = { is_installing: true, pkg_path: 'live:abc', progress: 10, downloaded_bytes: 1, total_bytes: 10 };
  assert.equal(monitorFor({}, live).ongoingInstall, live);
  assert.equal(monitorFor({ running: true }, live).ongoingInstall, null);
  assert.equal(monitorFor({}, { is_installing: true, pkg_path: '/mnt/usb0/a.pkg' }).ongoingInstall, null);
});

test('the monitor exposes the queue overview and actions', () => {
  const monitor = monitorFor({}, {});
  assert.equal(monitor.overview.active, null);
  assert.equal(monitor.activeStalled, false);
  for (const action of ['cancelQueue', 'skipCurrent', 'cancelOngoing']) assert.equal(typeof monitor[action], 'function');
});

test('a stall is tied to the package and progress it was measured on', async () => {
  const { stallMark } = await import('../src/hooks/useInstallStall.js');
  const status = { is_installing: true, pkg_path: 'live:fc27', progress: 12, downloaded_bytes: 100, total_bytes: 1000 };
  const mark = stallMark(status);
  assert.ok(mark);
  // The next queued package starts with a different mark, so an earlier stall cannot carry over.
  assert.notEqual(stallMark({ ...status, pkg_path: 'live:ghost' }), mark);
  assert.notEqual(stallMark({ ...status, downloaded_bytes: 101 }), mark);
  assert.equal(stallMark({ ...status, is_installing: false }), '');
  assert.equal(stallMark({ ...status, progress: -1 }), '');
  assert.equal(stallMark({ ...status, downloaded_bytes: 1000 }), '');
});

test('Stop and Skip cancel only the install streaming from this tab', () => {
  const status = { is_installing: true, pkg_path: 'live:other', progress: 10, downloaded_bytes: 1, total_bytes: 10 };
  for (const action of ['cancelQueue', 'skipCurrent']) {
    let canceled = 0;
    const queue = { running: true, stop: noop, skip: noop };
    const cancelInstall = () => { canceled++; };
    monitorFor(queue, status, { upload: { sessionId: 'mine' }, cancelInstall })[action]();
    assert.equal(canceled, 0, `${action} left another window's install alone`);
    monitorFor(queue, { ...status, pkg_path: 'live:mine' }, { upload: { sessionId: 'mine' }, cancelInstall })[action]();
    assert.equal(canceled, 1, `${action} canceled this tab's install`);
  }
});
