export async function initUpload(filename, total, owner, sessionId, details) {
  const res = await fetch('/api/upload/init', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ filename, total, owner, session_id: sessionId || '', ...details })
  });
  const data = await res.json();
  if (!res.ok || !data.success) {
    throw new Error((data && data.error) || ('Init failed: ' + res.status));
  }
  return data;
}

export async function checkUploadEligibility(details) {
  const res = await fetch('/api/upload/check', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(details)
  });
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || 'Could not check package installation');
  return data;
}

export async function uploadSessionIcon(owner, sessionId, icon) {
  const res = await fetch('/api/upload/icon', {
    method: 'POST',
    headers: {
      'Content-Type': 'image/png',
      'X-Direct-Owner': owner,
      'X-Direct-Session': sessionId
    },
    body: icon
  });
  if (!res.ok) throw new Error('Could not share package icon');
}

export async function uploadStatus() {
  const res = await fetch('/api/upload/status');
  if (!res.ok) throw new Error('Status failed: ' + res.status);
  return res.json();
}

export async function finishUpload() {
  const res = await fetch('/api/upload/finish', { method: 'POST' });
  const data = await res.json();
  if (!res.ok || !data.success) {
    throw new Error((data && data.error) || ('Finish failed: ' + res.status));
  }
  return data;
}

export async function cancelUpload(owner, sessionId) {
  const res = await fetch('/api/upload/cancel', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ owner, session_id: sessionId })
  });
  if (!res.ok) throw new Error('Cancel failed: ' + res.status);
  return res.json();
}

export function wsUploadUrl(wsPort) {
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
  return proto + '://' + window.location.hostname + ':' + wsPort + '/ws/upload';
}
