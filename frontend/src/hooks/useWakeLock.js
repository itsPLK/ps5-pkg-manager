import { useEffect, useState } from 'react';

// Keeps the screen awake while `active`. Returns whether a lock is held; browsers
// only grant one on secure pages (https or localhost) while the page is visible.
export function useWakeLock(active) {
  const [held, setHeld] = useState(false);
  useEffect(() => {
    if (!active || !window.isSecureContext || !navigator.wakeLock) return undefined;
    let lock = null;
    let requesting = false;
    let stopped = false;
    const acquire = async () => {
      if (requesting || document.visibilityState !== 'visible' || (lock && !lock.released)) return;
      requesting = true;
      try {
        const next = await navigator.wakeLock.request('screen');
        if (stopped) {
          next.release().catch(() => {});
          return;
        }
        lock = next;
        setHeld(true);
        lock.addEventListener('release', () => setHeld(false));
      } catch (e) {
        setHeld(false);
      } finally {
        requesting = false;
      }
    };
    acquire();
    document.addEventListener('visibilitychange', acquire);
    return () => {
      stopped = true;
      document.removeEventListener('visibilitychange', acquire);
      if (lock) lock.release().catch(() => {});
      setHeld(false);
    };
  }, [active]);
  return held;
}
