import { useState, useRef, useCallback, useEffect } from 'react';
import { checkUploadEligibility } from '../api/directInstall';
import { pollStatus } from '../api/installer';
import { parseLocalPkg } from '../utils/parseLocalPkg';
import { itemKey, isQueueable, pendingBaseTitles, runQueue, settleOutcome, summarizeRun } from '../utils/directQueue';

const PARSE_CONCURRENCY = 3;
const CONSOLE_POLL_MS = 3000;
// The console reports an install as stopped before its cleanup finishes, and
// that cleanup tears down any upload session created too soon after it.
const CONSOLE_SETTLE_MS = 5000;
// Longest wait for a console request between packages.
const REQUEST_TIMEOUT_MS = 5000;
// How long Stop or Skip waits for the run to react before ending it by force.
const UNRESPONSIVE_MS = 8000;
const GAME_STOPPED = 'Not installed: this game stopped installing on the PS5';

// How a finished attempt leaves the package in the list.
const AFTER_OUTCOME = {
  complete: { status: 'done' },
  failed: { status: 'failed' },
  skipped: { status: 'blocked' },
  canceled: { status: 'ready' },
};

function useLatest(value) {
  const ref = useRef(value);
  ref.current = value;
  return ref;
}

// Direct-install queue. The console accepts one upload session at a time, so
// queued packages stream one after another through `up` (useDirectUpload).
export function useDirectQueue(up) {
  const [items, setItems] = useState([]);
  const [running, setRunning] = useState(false);
  const [activeId, setActiveId] = useState('');
  const [skipped, setSkipped] = useState([]); // files that are not readable packages
  const [runIds, setRunIds] = useState([]); // packages attempted in the current or last run
  const [lastRun, setLastRun] = useState(null);
  const itemsRef = useLatest(items);
  const skippedRef = useLatest(skipped);
  const activeIdRef = useLatest(activeId);
  const upRef = useLatest(up);
  const runningRef = useRef(false);
  const stopRef = useRef(false);
  // Set by skip(): cancels only the active package. A reason marks it failed,
  // and `game` also takes the rest of its game out of the queue.
  const skipRef = useRef(null);
  // Identifies the run in progress. A run that stopped responding is replaced,
  // and whatever it does when it wakes up is ignored.
  const runRef = useRef(null);
  const wakeRef = useRef(() => {});
  const lastEndedAtRef = useRef(0);
  const removedRef = useRef(new Set());
  const startAfterQueueRef = useRef(false);
  const queueSeqRef = useRef(0);
  const nextSeq = () => ++queueSeqRef.current;

  useEffect(() => () => {
    itemsRef.current.forEach((item) => { if (item.iconUrl) URL.revokeObjectURL(item.iconUrl); });
  }, [itemsRef]);

  const patch = useCallback((id, changes) => {
    setItems((list) => list.map((item) => (item.id === id ? { ...item, ...changes } : item)));
  }, []);

  const updateWhere = useCallback((test, changes) => {
    setItems((list) => list.map((item) => (test(item) ? { ...item, ...changes(item) } : item)));
  }, []);

  const check = useCallback(async (item) => {
    try {
      const eligibility = await checkUploadEligibility(item.details);
      patch(item.id, { eligibility, status: eligibility.can_install ? 'ready' : 'blocked', error: '' });
      return eligibility;
    } catch (e) {
      patch(item.id, { status: 'error', error: e.message || 'Could not check package installation' });
      return null;
    }
  }, [patch]);

  const inspect = useCallback(async (item) => {
    let details;
    try {
      details = await parseLocalPkg(item.file);
    } catch (e) {
      setItems((list) => list.filter((other) => other.id !== item.id));
      setSkipped((list) => [...list, { id: item.id, path: item.path, error: e.message || 'Could not read package details' }]);
      return;
    }
    if (removedRef.current.has(item.id)) return;
    const iconUrl = details.icon_size
      ? URL.createObjectURL(item.file.slice(details.icon_offset, details.icon_offset + details.icon_size, 'image/png'))
      : '';
    patch(item.id, { details, iconUrl, status: 'checking' });
    await check({ ...item, details });
  }, [check, patch]);

  // `found`: [{ file, path }]; `unreadable`: [{ path, error }] entries that could not be opened.
  const addFiles = useCallback((found, unreadable = []) => {
    if (unreadable.length) {
      setSkipped((list) => [...list, ...unreadable.map((entry) => ({ id: `unreadable|${entry.path}`, ...entry }))]);
    }
    const known = new Set([...itemsRef.current, ...skippedRef.current].map((entry) => entry.id));
    const added = [];
    for (const entry of found) {
      const id = itemKey(entry);
      if (known.has(id)) continue;
      known.add(id);
      removedRef.current.delete(id);
      added.push({ id, file: entry.file, path: entry.path || entry.file.name, status: 'reading',
        details: null, eligibility: null, iconUrl: '', error: '', queued: false });
    }
    if (!added.length) return 0;
    setItems((list) => [...list, ...added]);
    const pending = [...added];
    const worker = async () => {
      while (pending.length) await inspect(pending.shift());
    };
    for (let i = 0; i < Math.min(PARSE_CONCURRENCY, added.length); i++) worker();
    return added.length;
  }, [inspect, itemsRef, skippedRef]);

  const removeWhere = useCallback((test) => {
    setItems((list) => list.filter((item) => {
      const drop = item.id !== activeIdRef.current && test(item);
      if (drop) {
        removedRef.current.add(item.id);
        if (item.iconUrl) URL.revokeObjectURL(item.iconUrl);
      }
      return !drop;
    }));
  }, [activeIdRef]);

  const remove = useCallback((id) => removeWhere((item) => item.id === id), [removeWhere]);

  const clear = useCallback(() => {
    setSkipped([]);
    removeWhere(() => true);
  }, [removeWhere]);

  const dismissSkipped = useCallback(() => setSkipped([]), []);

  const setQueued = useCallback((id, queued) => {
    patch(id, queued ? { queued, queuedAt: nextSeq() } : { queued });
  }, [patch]);

  // Queues the installable packages among `ids`, in list order.
  const queueAll = useCallback((ids) => {
    const only = new Set(ids);
    setItems((list) => {
      const pendingBases = pendingBaseTitles(list);
      return list.map((item) => (only.has(item.id) && isQueueable(item, pendingBases)
        ? { ...item, queued: true, queuedAt: nextSeq() } : item));
    });
  }, []);

  // Takes `ids` out of the queue, except the package installing now.
  const unqueue = useCallback((ids) => {
    const only = new Set(ids);
    updateWhere((item) => only.has(item.id) && item.queued && item.id !== activeIdRef.current, () => ({ queued: false }));
  }, [activeIdRef, updateWhere]);

  // Checks a failed package again and queues it if it can now be installed.
  const retry = useCallback(async (id) => {
    const item = itemsRef.current.find((other) => other.id === id);
    if (!item?.details) return;
    patch(id, { status: 'checking', error: '' });
    const eligibility = await check(item);
    if (eligibility?.can_install) patch(id, { queued: true, queuedAt: nextSeq() });
  }, [check, itemsRef, patch]);

  // Sleeps up to `ms`; Stop and Skip end it early.
  const pause = useCallback((ms) => new Promise((resolve) => {
    const timer = setTimeout(resolve, Math.max(0, ms));
    wakeRef.current = () => { clearTimeout(timer); resolve(); };
  }), []);

  // `request` or undefined once it takes too long or Stop or Skip is pressed.
  const bounded = useCallback((request) => Promise.race([
    request.catch(() => undefined),
    pause(REQUEST_TIMEOUT_MS),
  ]), [pause]);

  // Waits until the console is free (another tab or a library install may hold
  // it) and settled after the previous package. False if stopped or skipped.
  const waitForConsole = useCallback(async (id, isCurrent) => {
    const interrupted = () => stopRef.current || Boolean(skipRef.current) || !isCurrent();
    while (!interrupted()) {
      const status = await bounded(pollStatus());
      if (interrupted()) break;
      if (!status?.is_installing) {
        await pause(lastEndedAtRef.current + CONSOLE_SETTLE_MS - Date.now());
        return !interrupted();
      }
      patch(id, { status: 'waiting' });
      await pause(CONSOLE_POLL_MS);
    }
    return false;
  }, [bounded, patch, pause]);

  const installOne = useCallback(async (next, run) => {
    const isCurrent = () => runRef.current === run;
    skipRef.current = null;
    setRunIds((list) => [...list, next.id]);
    setActiveId(next.id);
    // Release any session left from an earlier upload in this tab, e.g. after a reload.
    await bounded(upRef.current.cancel());
    if (!isCurrent()) return { outcome: 'canceled', error: '' };
    upRef.current.reset();
    const attempt = await waitForConsole(next.id, isCurrent) && isCurrent()
      ? (patch(next.id, { status: 'installing', error: '' }), await upRef.current.upload(next))
      : { outcome: 'canceled', error: '' };
    if (!isCurrent()) return { outcome: 'canceled', error: '' };
    const result = settleOutcome(attempt, { stopped: stopRef.current, skip: skipRef.current });
    patch(next.id, { ...AFTER_OUTCOME[result.outcome], queued: false, error: result.error || '' });
    const titleId = next.details?.title_id;
    if (skipRef.current?.game && titleId) {
      updateWhere((item) => item.queued && item.details?.title_id === titleId, () => ({ queued: false, error: GAME_STOPPED }));
    }

    await bounded(upRef.current.cancel());
    if (!isCurrent()) return result;
    upRef.current.reset();
    lastEndedAtRef.current = Date.now();
    setActiveId('');
    // A new base can make its update or DLC installable.
    await bounded(Promise.all(itemsRef.current
      .filter((item) => item.id !== next.id && item.details?.title_id === titleId && ['ready', 'blocked'].includes(item.status))
      .map(check)));
    return result;
  }, [bounded, check, itemsRef, patch, upRef, updateWhere, waitForConsole]);

  const endRun = useCallback((results) => {
    runRef.current = null;
    runningRef.current = false;
    setRunning(false);
    setActiveId('');
    setLastRun({ ...summarizeRun(results), stopped: stopRef.current });
  }, []);

  // Installs queued packages until none is left, the run is stopped, or `run` is
  // replaced by a pass that continues it.
  const runPass = useCallback(async (run) => {
    try {
      await runQueue({
        getItems: () => itemsRef.current,
        install: async (next) => {
          const result = await installOne(next, run);
          if (runRef.current === run) run.results.push({ id: next.id, ...result });
          return result;
        },
        shouldStop: () => stopRef.current || runRef.current !== run,
        onlyId: run.onlyId,
        attempted: run.attempted,
      });
    } finally {
      if (runRef.current === run) endRun(run.results);
    }
  }, [endRun, installOne, itemsRef]);

  // `onlyId` installs just that package, ignoring the rest of the queue.
  const start = useCallback(async (onlyId) => {
    if (runningRef.current) return;
    const run = { onlyId, attempted: new Set(), results: [] };
    runRef.current = run;
    runningRef.current = true;
    stopRef.current = false;
    setRunning(true);
    setRunIds([]);
    setLastRun(null);
    await runPass(run);
  }, [runPass]);

  // Gives up on an active package that did not react to Stop or Skip in time, e.g.
  // one waiting on a console request that never returns. Its late actions are
  // ignored; after a Skip the same run carries on with a new pass.
  const forceEnd = useCallback((run, id, outcome, andContinue) => {
    if (runRef.current !== run || activeIdRef.current !== id) return;
    const result = settleOutcome({ outcome, error: '' }, { stopped: stopRef.current, skip: skipRef.current });
    patch(id, { ...AFTER_OUTCOME[result.outcome], queued: false, error: result.error || '' });
    run.results.push({ id, ...result });
    upRef.current.reset();
    lastEndedAtRef.current = Date.now();
    if (!andContinue) {
      endRun(run.results);
      return;
    }
    const pass = { ...run };
    runRef.current = pass;
    setActiveId('');
    runPass(pass);
  }, [activeIdRef, endRun, patch, runPass, upRef]);

  const installNow = useCallback((id) => start(id), [start]);

  // Queues `ids`, then starts once the queued flags have rendered.
  const startAll = useCallback((ids) => {
    const list = itemsRef.current;
    const pendingBases = pendingBaseTitles(list);
    const wanted = new Set(ids);
    if (list.some((item) => wanted.has(item.id) && !item.queued && isQueueable(item, pendingBases))) {
      startAfterQueueRef.current = true;
      queueAll(ids);
    } else {
      start();
    }
  }, [itemsRef, queueAll, start]);

  useEffect(() => {
    if (!startAfterQueueRef.current) return;
    startAfterQueueRef.current = false;
    start();
  }, [items, start]);

  // Asks the active package to stop, and ends the run by force if it does not react.
  const interrupt = useCallback((andContinue) => {
    const run = runRef.current;
    const id = activeIdRef.current;
    wakeRef.current();
    if (run && id) setTimeout(() => forceEnd(run, id, 'canceled', andContinue), UNRESPONSIVE_MS);
    return upRef.current.cancel();
  }, [activeIdRef, forceEnd, upRef]);

  const stop = useCallback(async () => {
    stopRef.current = true;
    if (!runningRef.current) return;
    await interrupt(false);
  }, [interrupt]);

  // Cancels the active package and continues. With a reason it is marked failed;
  // { game: true } also unqueues the rest of its game.
  const skip = useCallback(async (reason = '', { game = false } = {}) => {
    if (!runningRef.current || !activeIdRef.current) return;
    skipRef.current = { reason: typeof reason === 'string' ? reason : '', game };
    await interrupt(true);
  }, [activeIdRef, interrupt]);

  const dismissLastRun = useCallback(() => setLastRun(null), []);

  return { items, skipped, running, activeId, runIds, lastRun, addFiles, remove, clear, dismissSkipped, setQueued,
    queueAll, unqueue, retry, start, startAll, installNow, stop, skip, dismissLastRun };
}
