import assert from 'node:assert/strict';
import test from 'node:test';
import { collectDroppedPkgFiles, collectInputPkgFiles } from '../src/utils/collectPkgFiles.js';

const fakeFile = (name, size = 10, webkitRelativePath = '') => ({ name, size, webkitRelativePath });

function fileEntry(fullPath, size = 10) {
  const name = fullPath.split('/').pop();
  return { isFile: true, isDirectory: false, fullPath, file: (ok) => ok(fakeFile(name, size)) };
}

// Real readers return children in batches and an empty batch at the end.
function dirEntry(fullPath, children, batch = 2) {
  return {
    isFile: false, isDirectory: true, fullPath,
    createReader() {
      let offset = 0;
      return { readEntries(ok) { const next = children.slice(offset, offset + batch); offset += batch; ok(next); } };
    },
  };
}

test('dropped folders are walked recursively through every readEntries batch', async () => {
  const tree = dirEntry('/Game', [
    fileEntry('/Game/base.pkg'),
    fileEntry('/Game/readme.txt'),
    fileEntry('/Game/._base.pkg'),
    dirEntry('/Game/DLC', [fileEntry('/Game/DLC/a.pkg'), fileEntry('/Game/DLC/b.PKG'), fileEntry('/Game/DLC/empty.pkg', 0)]),
    fileEntry('/Game/update.pkg'),
  ]);
  const { files, unreadable } = await collectDroppedPkgFiles({ items: [{ kind: 'file', webkitGetAsEntry: () => tree }] });
  assert.deepEqual(unreadable, []);
  assert.deepEqual(files.map((entry) => entry.path).sort(),
    ['Game/DLC/a.pkg', 'Game/DLC/b.PKG', 'Game/base.pkg', 'Game/update.pkg']);
});

test('drops without entry support fall back to the plain file list', async () => {
  const { files } = await collectDroppedPkgFiles({ items: [], files: [fakeFile('x.pkg'), fakeFile('y.iso')] });
  assert.deepEqual(files.map((entry) => entry.path), ['x.pkg']);
});

test('folder picker results keep their relative paths', () => {
  const found = collectInputPkgFiles([fakeFile('a.pkg', 10, 'Games/a.pkg'), fakeFile('notes.txt')]);
  assert.deepEqual(found.map((entry) => entry.path), ['Games/a.pkg']);
});

test('an unreadable file or subfolder is reported without losing its siblings', async () => {
  const locked = { isFile: true, isDirectory: false, fullPath: '/Game/locked.pkg', file: (ok, fail) => fail(new Error('NotReadableError')) };
  const denied = { isFile: false, isDirectory: true, fullPath: '/Game/Denied',
    createReader: () => ({ readEntries: (ok, fail) => fail(new Error('SecurityError')) }) };
  const tree = dirEntry('/Game', [locked, denied, fileEntry('/Game/after.pkg')]);
  const { files, unreadable } = await collectDroppedPkgFiles({ items: [{ kind: 'file', webkitGetAsEntry: () => tree }] });
  assert.deepEqual(files.map((entry) => entry.path), ['Game/after.pkg']);
  assert.deepEqual(unreadable, [
    { path: 'Game/locked.pkg', error: 'NotReadableError' },
    { path: 'Game/Denied/', error: 'SecurityError' },
  ]);
});
