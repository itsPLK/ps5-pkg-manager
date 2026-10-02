import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { build } from 'esbuild';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

const require = createRequire(import.meta.url);
const bundle = await build({
  entryPoints: [fileURLToPath(new URL('../src/components/layout/DirectQueueBar.jsx', import.meta.url))],
  bundle: true, write: false, format: 'esm', platform: 'node',
  plugins: [{ name: 'shared-react', setup(builder) {
    builder.onResolve({ filter: /^react$/ }, () => ({ path: pathToFileURL(require.resolve('react')).href, external: true }));
  } }],
});
const { default: Bar } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`);

const idle = { active: null, progress: { label: '', percent: 0 }, overall: { count: 0, position: 0, percent: 0, etaSeconds: null } };
const render = (props) => renderToStaticMarkup(React.createElement(Bar, { overview: idle, onOpen: () => {}, ...props }));

test('the header bar shows an ongoing installation this tab does not own', () => {
  const html = render({ ongoing: { title_name: 'EA SPORTS FC 27', progress: 47.6 } });
  assert.match(html, /Ongoing installation/);
  assert.match(html, /EA SPORTS FC 27/);
  assert.match(html, /47\.6%/);
});

test('a stalled ongoing installation is flagged in the header bar', () => {
  const html = render({ ongoing: { title_name: 'EA SPORTS FC 27', progress: 47.6 }, stalled: true });
  assert.match(html, /Stalled: no data received/);
});

test('the header bar renders nothing without a queue or ongoing installation', () => {
  assert.equal(render({}), '');
});
