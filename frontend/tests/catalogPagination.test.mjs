import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';
import { build } from 'esbuild';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';

const require = createRequire(import.meta.url);
const bundle = await build({
  entryPoints: [fileURLToPath(new URL('../src/components/views/PackageGridView.jsx', import.meta.url))],
  bundle: true, write: false, format: 'esm', platform: 'node',
  plugins: [{ name: 'shared-react', setup(builder) {
    builder.onResolve({ filter: /^react$/ }, () => ({ path: pathToFileURL(require.resolve('react')).href, external: true }));
  } }],
});
const { default: Grid } = await import(`data:text/javascript;base64,${Buffer.from(bundle.outputFiles[0].text).toString('base64')}`);
const groups = Array.from({ length: 3000 }, (_, i) => ({
  id: String(i), title_id: `CUSA${90000 + i}`, title_name: `Title ${i}`,
  updates: [], totalSize: 1024,
}));
const render = (page, groupedTitles = groups) => renderToStaticMarkup(React.createElement(Grid, {
  groupedTitles, page, searchQuery: '', sortBy: 'title', selectedDrive: { id: 'smb', label: 'NAS' },
  settings: {}, installerStatus: {}, packages: groups,
}));

test('3,000 titles render only 60 cards, and the last title remains reachable', () => {
  const first = render(0);
  const last = render(49);
  assert.equal((first.match(/<h3 /g) || []).length, 60);
  assert.equal((last.match(/<h3 /g) || []).length, 60);
  assert.match(first, /title="Title 0"/);
  assert.doesNotMatch(first, /title="Title 60"/);
  assert.match(last, /title="Title 2999"/);
});

test('shrinking results clamps the visible page to avoid a blank catalog', () => {
  const html = render(49, groups.slice(0, 3));
  assert.equal((html.match(/<h3 /g) || []).length, 3);
  assert.match(html, /title="Title 0"/);
});
