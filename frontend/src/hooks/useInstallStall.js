import { useEffect, useRef, useState } from 'react';
import { isDirectStorage } from '../utils/directQueue.js';

const CHECK_MS = 5000;

// Identifies an install and how far it has read; '' when progress is not tracked.
// Direct-storage installs (no byte progress) and finishing ones are not tracked.
export function stallMark(status) {
  const tracked = status?.is_installing && !isDirectStorage(status) &&
    !(status.total_bytes > 0 && status.downloaded_bytes >= status.total_bytes);
  return tracked ? `${status.pkg_path || ''}|${status.downloaded_bytes}` : '';
}

// True once an install's byte progress has not moved for `ms`, e.g. a direct
// install whose sending tab was closed or whose download was canceled on the
// PS5. A stall belongs to the progress it was measured on, so a new package or
// any progress clears it in the same render.
export function useInstallStall(status, ms = 60000) {
  const mark = stallMark(status);
  const [stalledMark, setStalledMark] = useState('');
  const lastRef = useRef({ mark: '', at: 0 });

  if (lastRef.current.mark !== mark) lastRef.current = { mark, at: Date.now() };

  useEffect(() => {
    const timer = setInterval(() => {
      const last = lastRef.current;
      setStalledMark(last.mark && Date.now() - last.at > ms ? last.mark : '');
    }, CHECK_MS);
    return () => clearInterval(timer);
  }, [ms]);

  return Boolean(mark) && stalledMark === mark;
}
