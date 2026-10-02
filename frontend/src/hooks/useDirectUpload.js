import { useState, useRef, useCallback, useEffect } from 'react';
import { initUpload, uploadStatus, cancelUpload, cancelUploadOnUnload, checkUploadEligibility, uploadSessionIcon, wsUploadUrl } from '../api/directInstall';
import { pollStatus, installPackage } from '../api/installer';
import { createSegmentSender } from '../utils/segmentSender';

function getOwner() {
  let owner = sessionStorage.getItem('directInstallOwner');
  if (!owner) {
    const bytes = new Uint8Array(32);
    crypto.getRandomValues(bytes);
    owner = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
    sessionStorage.setItem('directInstallOwner', owner);
  }
  return owner;
}

function waitForMessage(ws, timeoutMs) {
  return new Promise(function (resolve, reject) {
    const timer = setTimeout(function () {
      ws.onmessage = null;
      ws.onerror = null;
      ws.onclose = null;
      reject(new Error('Upload timed out waiting for server'));
    }, timeoutMs || 30000);
    ws.onmessage = function (ev) {
      clearTimeout(timer);
      ws.onclose = null;
      try {
        resolve(JSON.parse(ev.data));
      } catch (e) {
        reject(new Error('Bad server reply'));
      }
    };
    ws.onerror = function () {
      clearTimeout(timer);
      reject(new Error('WebSocket error'));
    };
    // A server-side close (e.g. handshake reject, protocol error) raises
    // onclose, NOT onerror. Without this every such failure masquerades
    // as a 60s timeout.
    ws.onclose = function (ev) {
      clearTimeout(timer);
      reject(new Error('Server closed connection (code ' + (ev && ev.code) + ')'));
    };
  });
}

export function useDirectUpload(tabId) {
  const [state, setState] = useState('idle');
  const [uploadSpeed, setUploadSpeed] = useState(0);
  const [offset, setOffset] = useState(0);
  const [total, setTotal] = useState(0);
  const [sessionId, setSessionId] = useState('');
  const [iconUrl, setIconUrl] = useState('');
  const [installing, setInstalling] = useState(false);
  const sessionIdRef = useRef(sessionStorage.getItem('directInstallSession') || '');
  const installCalledRef = useRef(false);
  const installStartedRef = useRef(false);
  const cancelRef = useRef(false);
  // Set only by cancel(), unlike an internal abort such as a failed install start.
  const userCanceledRef = useRef(false);
  const startErrorRef = useRef('');
  // Numbers each upload; cancel() and reset() retire the one in progress so it
  // cannot touch the session of an upload started after it.
  const attemptRef = useRef(0);
  const wsRef = useRef(null);
  const statusTimerRef = useRef(null);
  const checkingRef = useRef(false);
  const uploadStatusInFlightRef = useRef(false);
  const unloadCancelSentRef = useRef(false);
  const speedTimerRef = useRef(null);
  const uploadRateRef = useRef({ samples: [], lastAt: 0 });

  useEffect(function () {
    const cancelOnUnload = function () {
      if (!installStartedRef.current || cancelRef.current || unloadCancelSentRef.current) return;
      const sid = sessionIdRef.current;
      if (!sid) return;
      unloadCancelSentRef.current = true;
      cancelUploadOnUnload(getOwner(), sid);
    };
    window.addEventListener('pagehide', cancelOnUnload);
    return function () {
      window.removeEventListener('pagehide', cancelOnUnload);
      if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    };
  }, []);

  const stopStatusPoll = useCallback(function () {
    if (statusTimerRef.current) {
      clearInterval(statusTimerRef.current);
      statusTimerRef.current = null;
    }
  }, []);

  const reset = useCallback(function () {
    attemptRef.current++;
    checkingRef.current = false;
    cancelRef.current = false;
    unloadCancelSentRef.current = false;
    stopStatusPoll();
    if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    speedTimerRef.current = null;
    setUploadSpeed(0);
    if (wsRef.current) {
      try { wsRef.current.close(); } catch (e) {}
      wsRef.current = null;
    }
    setState('idle');
    setOffset(0);
    setTotal(0);
    setSessionId('');
    setInstalling(false);
    installStartedRef.current = false;
    sessionIdRef.current = '';
    sessionStorage.removeItem('directInstallSession');
    setIconUrl('');
  }, [stopStatusPoll]);

  const cancel = useCallback(async function () {
    attemptRef.current++;
    checkingRef.current = false;
    cancelRef.current = true;
    userCanceledRef.current = true;
    stopStatusPoll();
    if (wsRef.current) {
      try { wsRef.current.close(); } catch (e) {}
      wsRef.current = null;
    }
    if (sessionIdRef.current) {
      try { await cancelUpload(getOwner(), sessionIdRef.current); } catch (e) {}
    }
    sessionIdRef.current = '';
    sessionStorage.removeItem('directInstallSession');
    setSessionId('');
    setInstalling(false);
    setState('canceled');
  }, [stopStatusPoll]);

  // Polls server upload status for header_ready (install can start as soon
  // as the header is parsed, mid-upload) and keeps offset fresh.
  const pollHeader = useCallback(function (sid, isStale) {
    stopStatusPoll();
    // Ends the upload with `message`; upload() reports it as a failure.
    const fail = function (message) {
      if (isStale()) return;
      stopStatusPoll();
      if (cancelRef.current) return;
      startErrorRef.current = message;
      cancelRef.current = true;
      if (wsRef.current) wsRef.current.close();
    };
    const tick = async function () {
      if (uploadStatusInFlightRef.current) return;
      uploadStatusInFlightRef.current = true;
      try {
        const st = await uploadStatus();
        if (isStale()) return;
        // The session can vanish, e.g. torn down by a previous install's cleanup.
        if (st && sid && (!st.active || (st.session_id && st.session_id !== sid))) {
          fail('The console dropped this upload before installing it. Try it again.');
          return;
        }
        if (st && typeof st.received === 'number') {
          setOffset(st.received);
        }
        if (st && st.header_ready) {
          stopStatusPoll();
          if (!installCalledRef.current && !cancelRef.current) {
            installCalledRef.current = true;
            try {
              const current = await pollStatus();
              if (isStale()) return;
              if (!(current?.is_installing && current.pkg_path === 'live:' + sid)) {
                await installPackage('live:' + sid);
              }
              setInstalling(true);
              installStartedRef.current = true;
            } catch (e) {
              fail(e.message || 'Could not start installation');
              await cancelUpload(getOwner(), sid).catch(function () {});
            }
          }
        }
      } catch (e) {} finally {
        uploadStatusInFlightRef.current = false;
      }
    };
    tick();
    statusTimerRef.current = setInterval(tick, 2000);
  }, [stopStatusPoll]);

  // Streams one parsed package ({ file, details, iconUrl }; the caller owns iconUrl).
  // Resolves to { outcome: 'complete' | 'skipped' | 'failed' | 'canceled', error }.
  const upload = useCallback(async function (item) {
    if (!item || !item.file || !item.details || checkingRef.current) return { outcome: 'skipped', error: '' };
    const file = item.file;
    const pkg = item.details;
    const canceled = { outcome: 'canceled', error: '' };
    const attempt = ++attemptRef.current;
    const isStale = function () { return attemptRef.current !== attempt; };
    const halted = function () { return isStale() || cancelRef.current; };
    cancelRef.current = false;
    userCanceledRef.current = false;
    startErrorRef.current = '';
    setIconUrl(item.iconUrl || '');
    checkingRef.current = true;
    setState('checking');
    try {
      const checked = await checkUploadEligibility(pkg);
      if (isStale() || userCanceledRef.current) return canceled;
      if (!checked.can_install) {
        setState('idle');
        return { outcome: 'skipped', error: checked.install_disabled_reason || '' };
      }
    } catch (e) {
      if (isStale()) return canceled;
      setState('error');
      return { outcome: 'failed', error: e.message || 'Could not check package installation' };
    } finally {
      if (!isStale()) checkingRef.current = false;
    }
    try {
      const lease = JSON.parse(localStorage.getItem('directInstallLease') || '{}');
      if (lease.tab && lease.tab !== tabId && Date.now() - lease.time < 10000) {
        setState('error');
        return { outcome: 'failed', error: 'Direct Install is active in another window' };
      }
    } catch (e) {}
    setTotal(file.size);
    setState('uploading');
    setUploadSpeed(0);
    uploadRateRef.current = { samples: [], lastAt: 0 };
    if (speedTimerRef.current) clearInterval(speedTimerRef.current);
    speedTimerRef.current = setInterval(function () {
      const now = Date.now();
      const rate = uploadRateRef.current;
      const recent = rate.samples.filter((sample) => now - sample.time <= 2000);
      rate.samples = recent;
      const activeUs = recent.reduce((sum, sample) => sum + sample.receiveUs, 0);
      const receivedBytes = recent.reduce((sum, sample) => sum + sample.bytes, 0);
      setUploadSpeed(rate.lastAt && now - rate.lastAt <= 1500 && activeUs > 0
        ? receivedBytes * 1000000 / activeUs : 0);
    }, 500);
    installCalledRef.current = false;
    installStartedRef.current = false;
    const SEG = 1024 * 1024;
    const NSEGS = Math.max(1, Math.ceil(file.size / SEG));
    // Coordinate bounded read-ahead with seek requests from installer
    // workers. Keep a bounded number in flight, retry busy writes, and leave the
    // socket open to serve seeks until the installation finalizes.
    let sender = null;
    let demandMode = false;
    let baselineSeg = 0;
    const ackedSet = new Set();
    let finished = false; // finish sent; only "complete" may arrive now
    let done = false;     // terminal: completed / failed / canceled
    let finishTimer = null;
    let watchdogTimer = null;
    let lastActivityAt = 0;
    let ws = null;
    let completionResolve = null;
    let completionReject = null;
    const completion = new Promise(function (res, rej) {
      completionResolve = res;
      completionReject = rej;
    });
    const fail = function (err) {
      if (!done) {
        done = true;
        completionReject(err instanceof Error ? err : new Error(String(err)));
      }
    };

    const recordReceiveRate = function (msg) {
      const bytes = Number(msg.rx_bytes);
      const receiveUs = Number(msg.rx_us);
      if (!Number.isFinite(bytes) || !Number.isFinite(receiveUs) || bytes <= 0 || receiveUs <= 0) return;
      const now = Date.now();
      const rate = uploadRateRef.current;
      rate.samples.push({ bytes, receiveUs, time: now });
      rate.samples = rate.samples.filter((sample) => now - sample.time <= 2000);
      rate.lastAt = now;
    };

    const installDispatcher = function () {
      lastActivityAt = Date.now();
      ws.onmessage = function (ev) {
        if (typeof ev.data !== 'string' || done || halted()) return;
        lastActivityAt = Date.now();
        let msg = null;
        try {
          msg = JSON.parse(ev.data);
        } catch (e) {
          fail(new Error('Bad server reply'));
          return;
        }
        if (!msg || typeof msg.op !== 'string') {
          fail(new Error('Bad server reply'));
          return;
        }
        if (msg.op === 'ack' && typeof msg.seg === 'number') {
          recordReceiveRate(msg);
          if (!ackedSet.has(msg.seg)) {
            ackedSet.add(msg.seg);
            setOffset(Math.min(file.size, ackedSet.size * SEG));
          }
          sender.ack(msg.seg);
        } else if (msg.op === 'busy' && typeof msg.seg === 'number') {
          recordReceiveRate(msg);
          sender.busy(msg.seg);
        } else if (msg.op === 'seek' && typeof msg.seg === 'number') {
          sender.request(msg.seg);
        } else if (msg.op === 'pong' && demandMode) {
          // Keep the socket alive while the installer is not requesting data.
        } else if (msg.op === 'complete') {
          done = true;
          completionResolve((msg && (msg.uri || msg.path)) || '');
        } else if (msg.op === 'error') {
          if (finished) {
            // Post-install cleanup race tolerance: accept a rejected
            // finish if the installer already went idle.
            pollStatus().then(function (st) {
              const idle = !st || !st.is_installing;
              if (idle && !done) {
                done = true;
                completionResolve('');
              } else {
                fail(new Error(msg.error || 'Server did not complete upload'));
              }
            }).catch(function () {
              if (!done) {
                done = true;
                completionResolve('');
              }
            });
          } else {
            fail(new Error(msg.error || 'Server rejected chunk'));
          }
        } else {
          fail(new Error('Bad server reply'));
        }
      };
      ws.onerror = function () {
        fail(new Error('WebSocket error'));
      };
      ws.onclose = function (ev) {
        // Fires on unexpected drops AND on our own post-complete close;
        // fail() no-ops once done.
        fail(new Error('Server closed connection (code ' + (ev && ev.code) + ')'));
      };
    };

    // Silent-socket watchdog: without it a dead connection (no TCP
    // close, e.g. dropped Wi-Fi) hangs the UI forever, since the
    // dispatcher itself has no timeouts. Demand mode uses a heartbeat
    // during installer pauses; legacy sequential mode exempts pure standby.
    // Pending work or a heartbeat must receive a reply within 60 s.
    const watchTraffic = function () {
      watchdogTimer = setInterval(function () {
        if (done || halted()) {
          if (watchdogTimer) {
            clearInterval(watchdogTimer);
            watchdogTimer = null;
          }
          return;
        }
        const pending = demandMode || sender.hasPending();
        if (demandMode && Date.now() - lastActivityAt > 15000 && ws.readyState === WebSocket.OPEN) {
          try { ws.send(JSON.stringify({ op: 'ping' })); } catch (e) { fail(e); }
        }
        if (pending && Date.now() - lastActivityAt > 60000) {
          fail(new Error('Upload stalled: no server reply for 60s'));
        }
      }, 5000);
    };

    // Keep serving seeks until the installer reports a terminal result.
    // A successful install may not consume every package segment.
    const watchFinish = function () {
      finishTimer = setInterval(async function () {
        if (done || finished || halted()) {
          if (finishTimer) {
            clearInterval(finishTimer);
            finishTimer = null;
          }
          return;
        }
        if (!installStartedRef.current) return;
        let st;
        try {
          st = await pollStatus();
        } catch (e) { return; }
        if (!st || st.is_installing || (!st.completed && !st.failed)) return;
        if (st.failed) {
          fail(new Error('Installation failed'));
          return;
        }
        sender.flushStats();
        setInstalling(false);
        if (ackedSet.size < NSEGS) {
          // The installer may finish without reading every package byte.
          // No more reader can free the RAM window, so stop the baseline.
          done = true;
          completionResolve('');
          return;
        }
        if (finishTimer) {
          clearInterval(finishTimer);
          finishTimer = null;
        }
        finished = true;
        try {
          ws.send(JSON.stringify({ op: 'finish' }));
        } catch (e) {
          fail(e);
          return;
        }
        setTimeout(function () {
          if (!done) fail(new Error('Server did not complete upload'));
        }, 60000);
      }, 2000);
    };

    // Stops this upload's own timers and socket. The shared status poll, speed
    // meter and socket ref are left alone once a newer upload owns them.
    const cleanup = function () {
      if (sender) sender.stop();
      if (finishTimer) clearInterval(finishTimer);
      if (watchdogTimer) clearInterval(watchdogTimer);
      finishTimer = null;
      watchdogTimer = null;
      if (ws) {
        try { ws.close(); } catch (e) {}
      }
      if (isStale()) return;
      stopStatusPoll();
      if (speedTimerRef.current) clearInterval(speedTimerRef.current);
      speedTimerRef.current = null;
      setUploadSpeed(0);
      wsRef.current = null;
    };

    try {
      const init = await initUpload(file.name, file.size, getOwner(), sessionIdRef.current, pkg);
      if (isStale() || userCanceledRef.current) {
        // Canceled while the session was being created: release it now.
        if (init.session_id) await cancelUpload(getOwner(), init.session_id).catch(function () {});
        throw new Error('Canceled');
      }
      baselineSeg = Math.ceil((init.offset || 0) / SEG);
      for (let s = 0; s < baselineSeg; s++) ackedSet.add(s);
      sessionIdRef.current = init.session_id || '';
      sessionStorage.setItem('directInstallSession', sessionIdRef.current);
      setSessionId(init.session_id || '');
      setOffset(Math.min(file.size, baselineSeg * SEG));
      if (init.session_id && pkg.icon_size) {
        const icon = file.slice(pkg.icon_offset, pkg.icon_offset + pkg.icon_size, 'image/png');
        try { await uploadSessionIcon(getOwner(), init.session_id, icon); } catch (e) {}
      }
      if (isStale() || userCanceledRef.current || cancelRef.current) throw new Error('Canceled');
      if (init.session_id) pollHeader(init.session_id, isStale);

      ws = new WebSocket(wsUploadUrl(init.ws_port || 18842));
      wsRef.current = ws;
      await new Promise(function (resolve, reject) {
        ws.onopen = resolve;
        ws.onerror = function () { reject(new Error('Cannot reach upload socket')); };
        ws.onclose = function (ev) { reject(new Error('Server closed connection (code ' + (ev && ev.code) + ')')); };
      });

      // Confirm over the socket (idempotent: same file+total resumes).
      ws.send(JSON.stringify({ op: 'init', filename: file.name, total: file.size,
        owner: getOwner(), session_id: init.session_id }));
      const ready = await waitForMessage(ws, 30000);
      if (ready.op === 'error') throw new Error(ready.error || 'Server refused upload');
      if (ready.op !== 'ready') throw new Error('Bad server reply to init');

      demandMode = Number.isInteger(ready.demand_window) && ready.demand_window > 0;
      sender = createSegmentSender({
        totalSegments: NSEGS,
        startSegment: baselineSeg,
        maxInFlight: ready.upload_window === 2 ? 2 : 1,
        onStats: (stats) => {
          if (ready.upload_window === 2 && ws.readyState === WebSocket.OPEN) {
            try { ws.send(JSON.stringify({ op: 'sender_stats', ...stats })); } catch (e) { fail(e); }
          }
        },
        demandWindow: Number.isInteger(ready.demand_window) ? Math.min(8, Math.max(0, ready.demand_window)) : 0,
        acknowledged: ackedSet,
        readSegment: (segment) => file.slice(segment * SEG, Math.min((segment + 1) * SEG, file.size)).arrayBuffer(),
        sendSegment: (segment, buffer) => {
          ws.send(JSON.stringify({ op: 'seg', seg: segment }));
          ws.send(buffer);
        },
        shouldStop: () => done || finished || halted(),
        onError: fail
      });
      installDispatcher();
      watchTraffic();
      watchFinish();
      await sender.pump();
      await completion;
      cleanup();
      if (isStale()) return canceled;
      setState('complete');
      return { outcome: 'complete', error: '' };
    } catch (e) {
      cleanup();
      if (isStale() || userCanceledRef.current) return canceled;
      setState('error');
      return { outcome: 'failed', error: startErrorRef.current || e.message || 'Upload failed' };
    }
  }, [pollHeader, tabId]);

  return { state, offset, total, uploadSpeed, sessionId, iconUrl, installing, upload, cancel, reset };
}
