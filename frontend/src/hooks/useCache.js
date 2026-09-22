import { useState } from 'react';
import { getCacheStats, clearCache } from '../api/cache';
import { formatBytes } from '../utils/formatters';

export function useCache(_ref) {
  var showToast = _ref.showToast;
  var onRescan = _ref.onRescan;
  const [cacheStats, setCacheStats] = useState(null);
  const [loadingStats, setLoadingStats] = useState(false);
  const [showClearCacheModal, setShowClearCacheModal] = useState(false);
  const [clearingCache, setClearingCache] = useState(false);

  const fetchCacheStats = async () => {
    setLoadingStats(true);
    try {
      const data = await getCacheStats();
      setCacheStats(data);
    } catch (err) {} finally {
      setLoadingStats(false);
    }
  };

  const handleClearCache = async () => {
    setClearingCache(true);
    try {
      const data = await clearCache();
      if (data.success) {
        showToast('Cache cleared! Freed ' + formatBytes(data.freed_bytes) + '.', 'success');
        setShowClearCacheModal(false);
        await Promise.all([
          fetchCacheStats(),
          onRescan ? onRescan() : Promise.resolve()
        ]);
      } else {
        showToast(data.error || 'Failed to clear cache', 'error');
      }
    } catch (err) {
      showToast('Clear cache failed: ' + err.message, 'error');
    } finally {
      setClearingCache(false);
    }
  };

  return {
    cacheStats, loadingStats, showClearCacheModal, setShowClearCacheModal,
    clearingCache, fetchCacheStats, handleClearCache
  };
}
