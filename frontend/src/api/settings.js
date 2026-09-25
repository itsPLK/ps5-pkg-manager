export async function getSettings() {
  const res = await fetch('/api/settings');
  if (!res.ok) throw new Error(`Settings fetch failed: ${res.status}`);
  return res.json();
}

export async function saveSettings(settings) {
  const res = await fetch('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(settings)
  });
  if (!res.ok) throw new Error(`Settings save failed: ${res.status}`);
  return res.json();
}

export async function installShortcut() {
  const res = await fetch('/api/shortcut/install', { method: 'POST' });
  if (!res.ok) throw new Error(`Shortcut install failed: ${res.status}`);
  return res.json();
}

export async function closeManager() {
  const res = await fetch('/api/shutdown', { method: 'POST' });
  if (!res.ok) throw new Error(`Close request failed: ${res.status}`);
  const result = await res.json();
  if (!result.success) throw new Error(result.error || 'Close request was rejected');

  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 200));
    try {
      await fetch('/api/version', { cache: 'no-store' });
    } catch (err) {
      return result;
    }
  }

  throw new Error('PKG Manager is still responding after the close request.');
}
