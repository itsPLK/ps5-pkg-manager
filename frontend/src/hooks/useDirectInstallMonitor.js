import { useEffect, useRef } from 'react';
import { describeRun, isDirectStorage, isLiveInstall, queueOverview } from '../utils/directQueue';
import { getBrowserTitle } from '../utils/title';
import { useInstallStall } from './useInstallStall';
import { useWakeLock } from './useWakeLock';

// A queued package with no progress warns after STALL_WARN_MS and its game is
// given up at ABANDON_MS: the console cannot tell when a download is canceled
// on the PS5 itself, it just stops reading.
const STALL_WARN_MS = 15 * 1000;
const ABANDON_MS = 45 * 1000;
const ABANDON_REASON = 'The PS5 stopped reading this package for 45 seconds. Was the download canceled on the console?';

// App-wide behavior around direct installs: progress for the page and header
// bar, stall handling, cancel actions, tab title, keep-awake and the run summary.
export function useDirectInstallMonitor({ queue, upload, installerStatus, speed, appVersion, cancelInstall, showToast }) {
  const live = isLiveInstall(installerStatus);
  // The install streaming from this tab's upload session, as opposed to one
  // left by another window or an earlier page.
  const own = live && Boolean(upload.sessionId) && installerStatus.pkg_path === `live:${upload.sessionId}`;
  const stalled = useInstallStall(installerStatus);
  const stallWarning = useInstallStall(installerStatus, STALL_WARN_MS);
  const abandoned = useInstallStall(installerStatus, ABANDON_MS);
  const keepAwake = useWakeLock(queue.running);
  const overview = queueOverview(queue, installerStatus, upload, speed);
  const ongoingInstall = live && !queue.running ? installerStatus : null;
  // useToast returns a new function every render.
  const showToastRef = useRef(showToast);
  showToastRef.current = showToast;

  // The console cancels streamed installs itself; direct-storage ones cannot be canceled.
  const cancelOwnInstall = () => {
    if (own && !isDirectStorage(installerStatus)) cancelInstall();
  };
  const cancelQueue = () => {
    cancelOwnInstall();
    queue.stop();
  };
  const skipCurrent = () => {
    cancelOwnInstall();
    queue.skip();
  };
  const cancelOngoing = () => {
    cancelInstall();
    if (live) upload.cancel();
  };

  useEffect(() => {
    if (!queue.running || !own || !abandoned) return;
    cancelInstall();
    queue.skip(ABANDON_REASON, { game: true });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [queue.running, own, abandoned]);

  const titlePercent = queue.running
    ? Math.floor(overview.overall.count > 1 ? overview.overall.percent : overview.progress.percent)
    : null;
  useEffect(() => {
    if (titlePercent === null) return undefined;
    document.title = `(${titlePercent}%) ${getBrowserTitle(appVersion)}`;
    return () => { document.title = getBrowserTitle(appVersion); };
  }, [titlePercent, appVersion]);

  useEffect(() => {
    const run = queue.lastRun;
    if (!run || !(run.complete + run.failed + run.skipped + run.canceled)) return;
    showToastRef.current(describeRun(run), run.failed ? 'error' : run.stopped ? 'warning' : 'success');
  }, [queue.lastRun]);

  return {
    overview,
    ongoingInstall,
    ongoingStalled: Boolean(ongoingInstall) && stalled,
    activeStalled: queue.running && own && stallWarning,
    keepAwake,
    cancelQueue,
    skipCurrent,
    cancelOngoing,
  };
}
