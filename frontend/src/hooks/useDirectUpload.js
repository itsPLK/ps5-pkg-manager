import { useState, useRef, useCallback } from 'react';
import { initUpload, uploadStatus, cancelUpload, wsUploadUrl } from '../api/directInstall';
import { pollStatus } from '../api/installer';

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

export function useDirectUpload() {
  const [state, setState] = useState('idle');
  const [progress, setProgress] = useState(0);
  const [offset, setOffset] = useState(0);
  const [total, setTotal] = useState(0);
  const [fileName, setFileName] = useState('');
  const [sessionId, setSessionId] = useState('');
  const [headerReady, setHeaderReady] = useState(false);
  const [installPath, setInstallPath] = useState('');
  const [error, setError] = useState('');
  const cancelRef = useRef(false);
  const wsRef = useRef(null);
  const statusTimerRef = useRef(null);

  const stopStatusPoll = useCallback(function () {
    if (statusTimerRef.current) {
      clearInterval(statusTimerRef.current);
      statusTimerRef.current = null;
    }
  }, []);

  const reset = useCallback(function () {
    cancelRef.current = false;
    stopStatusPoll();
    if (wsRef.current) {
      try { wsRef.current.close(); } catch (e) {}
      wsRef.current = null;
    }
    setState('idle');
    setProgress(0);
    setOffset(0);
    setTotal(0);
    setFileName('');
    setSessionId('');
    setHeaderReady(false);
    setInstallPath('');
    setError('');
  }, [stopStatusPoll]);

  const cancel = useCallback(async function () {
    cancelRef.current = true;
    stopStatusPoll();
    if (wsRef.current) {
      try { wsRef.current.close(); } catch (e) {}
      wsRef.current = null;
    }
    try { await cancelUpload(); } catch (e) {}
    setState('canceled');
  }, [stopStatusPoll]);

  // Polls server upload status for header_ready (install can start as soon
  // as the header is parsed, mid-upload) and keeps offset fresh.
  const pollHeader = useCallback(function (sid, fileSize) {
    stopStatusPoll();
    const tick = async function () {
      try {
        const st = await uploadStatus();
        if (st && st.session_id && sid && st.session_id !== sid) return;
        if (st && typeof st.received === 'number') {
          setOffset(st.received);
          setProgress(fileSize > 0 ? Math.round((st.received / fileSize) * 100) : 0);
        }
        if (st && st.header_ready) {
          setHeaderReady(true);
          setInstallPath('live:' + sid);
          stopStatusPoll();
        }
      } catch (e) {}
    };
    tick();
    statusTimerRef.current = setInterval(tick, 2000);
  }, [stopStatusPoll]);

  const upload = useCallback(async function (file) {
    if (!file) return;
    reset();
    cancelRef.current = false;
    setFileName(file.name);
    setTotal(file.size);
    setState('uploading');
    setError('');
    const SEG = 1024 * 1024;
    const NSEGS = Math.max(1, Math.ceil(file.size / SEG));
    // Virtual-block-device pusher (WS_fix_plan_2.md): FIFO seek queue
    // (two parallel installer workers can park at once -- a scalar
    // detour would drop one), stop-and-wait inflight guard, "busy"
    // requeue, and demand-paging standby: after the baseline is fully
    // acked the socket stays open serving seeks until the install
    // finalizes. Seeks are never dropped; the server re-emits them.
    const seekQueue = [];
    let inflightSeg = -1;
    let baselineSeg = 0;
    const ackedSet = new Set();
    let isPumping = false;
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

    const dispatchSegment = async function (s) {
      inflightSeg = s;
      const start = s * SEG;
      const end = Math.min(start + SEG, file.size);
      const buf = await file.slice(start, end).arrayBuffer();
      ws.send(JSON.stringify({ op: 'seg', seg: s }));
      ws.send(buf);
    };

    const pump = async function () {
      // Priority-based demand paging: parked-reader seeks preempt the
      // background baseline; with nothing to send we stand by (the
      // dispatcher re-fires us on every ack/busy/seek).
      if (isPumping || inflightSeg >= 0 || finished || done || cancelRef.current) return;
      isPumping = true;
      try {
        while (seekQueue.length > 0) {
          const s = seekQueue.shift();
          if (s < 0 || s >= NSEGS) continue;
          await dispatchSegment(s);
          return;
        }
        while (baselineSeg < NSEGS && ackedSet.has(baselineSeg)) baselineSeg++;
        if (baselineSeg < NSEGS) {
          const s = baselineSeg++;
          await dispatchSegment(s);
          return;
        }
        // Baseline fully acked: event-driven standby. Do NOT close --
        // the installer may still seek evicted segments (phase pivots).
      } catch (e) {
        fail(e);
      } finally {
        isPumping = false;
      }
    };

    const installDispatcher = function () {
      lastActivityAt = Date.now();
      ws.onmessage = function (ev) {
        if (typeof ev.data !== 'string' || done || cancelRef.current) return;
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
          if (!ackedSet.has(msg.seg)) {
            ackedSet.add(msg.seg);
            setOffset(Math.min(file.size, ackedSet.size * SEG));
            setProgress(Math.round((ackedSet.size / NSEGS) * 100));
          }
          if (inflightSeg === msg.seg) inflightSeg = -1;
          pump();
        } else if (msg.op === 'busy' && typeof msg.seg === 'number') {
          // Ring momentarily full of unserved data: requeue at the head
          // and retry shortly. The socket itself never wedges.
          if (inflightSeg === msg.seg) inflightSeg = -1;
          if (!ackedSet.has(msg.seg) && seekQueue.indexOf(msg.seg) < 0) {
            seekQueue.unshift(msg.seg);
          }
          setTimeout(function () { pump(); }, 50);
        } else if (msg.op === 'seek' && typeof msg.seg === 'number') {
          if (msg.seg >= 0 && msg.seg < NSEGS && seekQueue.indexOf(msg.seg) < 0) {
            seekQueue.push(msg.seg);
          }
          pump();
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
    // dispatcher itself has no timeouts. Pure standby (baseline fully
    // acked, nothing in flight or queued) is legitimately silent for
    // minutes while an install runs, so it is exempt; anything else
    // must show server traffic at least every 60 s.
    const watchTraffic = function () {
      watchdogTimer = setInterval(function () {
        if (done || cancelRef.current) {
          if (watchdogTimer) {
            clearInterval(watchdogTimer);
            watchdogTimer = null;
          }
          return;
        }
        const pending = ackedSet.size < NSEGS || inflightSeg >= 0 || seekQueue.length > 0;
        if (pending && Date.now() - lastActivityAt > 60000) {
          fail(new Error('Upload stalled: no server reply for 60s'));
        }
      }, 5000);
    };

    // Sends {op:finish} once the baseline is fully acked AND the
    // installer is no longer running. While an install runs we stay in
    // demand-paging standby (dispatcher serves seeks); if the user never
    // starts one, we finish right after the upload.
    const watchFinish = function () {
      finishTimer = setInterval(async function () {
        if (done || cancelRef.current) {
          if (finishTimer) {
            clearInterval(finishTimer);
            finishTimer = null;
          }
          return;
        }
        if (ackedSet.size < NSEGS) return;
        let running = false;
        try {
          const st = await pollStatus();
          running = !!(st && st.is_installing);
        } catch (e) {}
        if (running) return;
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

    try {
      const init = await initUpload(file.name, file.size);
      baselineSeg = Math.floor((init.offset || 0) / SEG);
      setSessionId(init.session_id || '');
      setOffset(baselineSeg * SEG);
      setProgress(file.size > 0 ? Math.round(((baselineSeg * SEG) / file.size) * 100) : 0);
      if (init.session_id) pollHeader(init.session_id, file.size);

      ws = new WebSocket(wsUploadUrl(init.ws_port || 8846));
      wsRef.current = ws;
      await new Promise(function (resolve, reject) {
        ws.onopen = resolve;
        ws.onerror = function () { reject(new Error('Cannot reach upload socket')); };
        ws.onclose = function (ev) { reject(new Error('Server closed connection (code ' + (ev && ev.code) + ')')); };
      });

      // Confirm over the socket (idempotent: same file+total resumes).
      ws.send(JSON.stringify({ op: 'init', filename: file.name, total: file.size }));
      const ready = await waitForMessage(ws, 30000);
      if (ready.op === 'error') throw new Error(ready.error || 'Server refused upload');
      if (ready.op !== 'ready') throw new Error('Bad server reply to init');

      installDispatcher();
      watchTraffic();
      watchFinish();
      await pump();
      await completion;
      if (finishTimer) {
        clearInterval(finishTimer);
        finishTimer = null;
      }
      if (watchdogTimer) {
        clearInterval(watchdogTimer);
        watchdogTimer = null;
      }
      stopStatusPoll();
      try { ws.close(); } catch (e) {}
      wsRef.current = null;

      if (init.session_id) setInstallPath('live:' + init.session_id);
      setState('complete');
    } catch (e) {
      if (finishTimer) {
        clearInterval(finishTimer);
        finishTimer = null;
      }
      if (watchdogTimer) {
        clearInterval(watchdogTimer);
        watchdogTimer = null;
      }
      stopStatusPoll();
      if (ws) {
        try { ws.close(); } catch (err) {}
        wsRef.current = null;
      }
      if (!cancelRef.current) {
        setError(e.message || 'Upload failed');
        setState('error');
      }
    }
  }, [reset, pollHeader]);

  return { state, progress, offset, total, fileName, sessionId, headerReady, installPath, error, upload, cancel, reset };
}
