export async function testSmb(config) {
  const res = await fetch('/api/smb/test', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(config)
  });
  if (!res.ok) throw new Error(`SMB test failed: ${res.status}`);
  return res.json();
}

export async function listSmbShares(config) {
  const res = await fetch('/api/smb/shares', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      id: config.id || '',
      server: config.server || '',
      port: config.port || 445,
      username: config.username || '',
      password: config.password || '',
      workgroup: config.workgroup || 'WORKGROUP'
    })
  });
  if (!res.ok) throw new Error(`SMB share list failed: ${res.status}`);
  return res.json();
}

export async function browseSmb(config) {
  const res = await fetch('/api/smb/browse', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      id: config.id || '',
      server: config.server || '',
      port: config.port || 445,
      username: config.username || '',
      password: config.password || '',
      workgroup: config.workgroup || 'WORKGROUP',
      share: config.share || '',
      path: config.path || ''
    })
  });
  if (!res.ok) throw new Error(`SMB browse failed: ${res.status}`);
  return res.json();
}
