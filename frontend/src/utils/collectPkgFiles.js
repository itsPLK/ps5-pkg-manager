// Collects .pkg files from a drop or file input, walking dropped folders.
// Resolves to { files: [{ file, path }], unreadable: [{ path, error }] }.

// Skips macOS "._" resource-fork companions.
const isPkgName = (name) => /\.pkg$/i.test(name) && !name.startsWith('._');
const isPkg = (file) => isPkgName(file.name) && file.size > 0;
const errorText = (e) => (e && (e.message || e.name)) || 'Could not be read';

// readEntries returns children in batches; call it until a batch is empty.
function readAll(reader) {
  return new Promise((resolve, reject) => {
    const all = [];
    const next = () => reader.readEntries((batch) => {
      if (!batch.length) resolve(all);
      else { all.push(...batch); next(); }
    }, reject);
    next();
  });
}

// An unreadable file or folder is reported and skipped; its siblings are still collected.
async function walk(entry, out) {
  const path = entry.fullPath.replace(/^\//, '');
  if (entry.isFile) {
    if (!isPkgName(entry.name || path.split('/').pop())) return;
    try {
      const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
      if (isPkg(file)) out.files.push({ file, path });
    } catch (e) {
      out.unreadable.push({ path, error: errorText(e) });
    }
  } else if (entry.isDirectory) {
    let children;
    try {
      children = await readAll(entry.createReader());
    } catch (e) {
      out.unreadable.push({ path: path + '/', error: errorText(e) });
      return;
    }
    for (const child of children) await walk(child, out);
  }
}

// Call synchronously in the drop handler: DataTransfer items are emptied once
// the event returns, so the entries are captured before any await.
export function collectDroppedPkgFiles(dataTransfer) {
  const entries = Array.from(dataTransfer?.items || [])
    .filter((item) => item.kind === 'file')
    .map((item) => item.webkitGetAsEntry?.())
    .filter(Boolean);
  if (!entries.length) {
    return Promise.resolve({ files: collectInputPkgFiles(dataTransfer?.files), unreadable: [] });
  }
  return (async () => {
    const out = { files: [], unreadable: [] };
    for (const entry of entries) await walk(entry, out);
    return out;
  })();
}

export function collectInputPkgFiles(fileList) {
  return Array.from(fileList || []).filter(isPkg).map((file) => ({ file, path: file.webkitRelativePath || file.name }));
}
