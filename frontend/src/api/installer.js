export async function pollStatus() {
  const res = await fetch('/api/poll');
  if (!res.ok) throw new Error(`Poll failed: ${res.status}`);
  return res.json();
}

export async function installPackage(path, updatePath = '') {
  const res = await fetch('/api/install', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(updatePath ? { path, update_path: updatePath } : { path })
  });
  if (!res.ok) throw new Error(`Install failed: ${res.status}`);
  return res.json();
}

export async function cancelInstall() {
  const res = await fetch('/api/cancel', { method: 'POST' });
  if (!res.ok) throw new Error(`Cancel failed: ${res.status}`);
  return res.json();
}
