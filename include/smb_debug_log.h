/*
 * PKG Manager - SMB Streaming Debug Logger
 *
 * Records timing, throughput and per-slot detail for every SMB read
 * made during a streaming install.  Produces a timestamped text file in
 * /data/pkgmgr/ (or $PKG_DEBUG_DIR) that can be compared with the
 * stream_debug_log to isolate where the Samba speed gap comes from.
 *
 *   /data/pkgmgr/smb_debug_<server>_<YYYYMMDD_HHMMSS>.txt
 *
 * Thread-safe: uses its own mutex.  Safe to call when not open.
 */
#ifndef SMB_DEBUG_LOG_H
#define SMB_DEBUG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* Monotonic microsecond clock used internally for slot RTT measurements.
 * Exposed so smb_client.c can timestamp issue/completion without a separate
 * gettimeofday call. */
uint64_t smb_dbg_now_us_internal(void);

/*
 * Open a new SMB debug log for one streaming session.
 * server     - NAS hostname/IP
 * share      - SMB share name
 * smb_url    - full smb:// path being streamed
 * file_size  - total file size in bytes
 * Returns 0 on success, -1 if the file could not be created (logging will
 * be silently disabled for this session).
 */
int smb_debug_log_open(const char *server, const char *share,
                       const char *smb_url, uint64_t file_size);

/*
 * Record one completed smb_file_session_read() call.
 * offset     - file offset requested
 * requested  - bytes the caller wanted
 * returned   - bytes actually delivered to the caller
 * elapsed_us - wall-clock microseconds the call took
 * n_slots    - number of async pipeline slots that fired
 */
void smb_debug_log_read(uint64_t offset, size_t requested,
                        ssize_t returned, uint64_t elapsed_us,
                        int n_slots);

/*
 * Record one async-slot completion inside smb_file_session_read().
 * slot_idx   - slot index (0 .. SMB_PIPELINE_DEPTH-1)
 * offset     - file offset this slot was fetching
 * requested  - bytes this slot requested from smb2_pread_async
 * returned   - bytes libsmb2 actually delivered (may be < requested when
 *              credit-limited)
 * rtt_us     - microseconds from smb2_pread_async call to callback fire
 */
void smb_debug_log_slot(int slot_idx, uint64_t offset,
                        uint32_t requested, int returned,
                        uint64_t rtt_us);

/*
 * Write a one-line summary (total bytes, wall time, average throughput)
 * and close the log file.  Safe to call when not open.
 */
void smb_debug_log_close(void);

#ifdef __cplusplus
}
#endif

#endif /* SMB_DEBUG_LOG_H */
