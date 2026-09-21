import React from 'react';
import BlurIcon, { iconUrlFor } from '../../BlurIcon';
import { formatBytes } from '../../utils/formatters';

export default function InstallingScreen({ installerStatus, batchInstall, etaInfo, storage, isDiscSource, onCancel, packages = [], directIconUrl }) {
const isBatch = !!(batchInstall && batchInstall.combinedTotal > 0);
    let totalBytes = installerStatus.total_bytes;
    let downloadedBytes = installerStatus.downloaded_bytes;
    let progressVal = installerStatus.progress;
    let titleToDisplay = installerStatus.title_name || 'Installing Package...';
    let displayIconPath = installerStatus.pkg_path || null;
    let displayIconPkg = (displayIconPath && packages)
      ? packages.find((p) => p.path === displayIconPath) || null
      : null;
    let statusChip = installerStatus.is_multipart
      ? `Installing Part ${installerStatus.current_part} of ${installerStatus.total_parts}`
      : 'Installing to PS5';

    if (isBatch) {
      totalBytes = batchInstall.combinedTotal;
      if (batchInstall.stage === 'base') {
        downloadedBytes = Math.min(batchInstall.baseSize, installerStatus.downloaded_bytes);
        statusChip = `Installing Base + Update (Part 1/2: Base Package)`;
      } else {
        downloadedBytes = batchInstall.baseSize + Math.min(batchInstall.updateSize, installerStatus.downloaded_bytes);
        statusChip = `Installing Base + Update (Part 2/2: Update)`;
      }
      progressVal = totalBytes > 0 ? (downloadedBytes / totalBytes) * 100 : 0;
      if (batchInstall.titleName) {
        titleToDisplay = batchInstall.titleName;
      }
      if (batchInstall.iconPath) {
        displayIconPath = batchInstall.iconPath;
        displayIconPkg = (packages && packages.find((p) => p.path === displayIconPath)) || null;
      }
    }
    const displayIconUrl = iconUrlFor(
      displayIconPath,
      displayIconPkg ? displayIconPkg.mtime : 0,
      displayIconPkg ? displayIconPkg.file_size : 0
    );

    return (
      <div className="fixed inset-0 z-50 bg-[#0a0a0f] text-white flex flex-col items-center justify-center p-6 overflow-hidden select-none">
        <div className="relative z-10 flex flex-col items-center max-w-4xl w-full text-center">
          {/* Status Chip */}
          <div className="px-3 py-1 rounded-[2px] bg-blue-500/20 text-blue-300 border border-blue-500/30 text-xs uppercase font-bold tracking-wider mb-5">
            {statusChip}
          </div>

          {/* Big Package Picture */}
          <div className="relative w-48 h-48 sm:w-60 sm:h-60 rounded-[2px] overflow-hidden bg-[#141520] border border-white/20 flex items-center justify-center">
            {/* Fallback Icon */}
            <div className="absolute inset-0 flex items-center justify-center text-zinc-600 pointer-events-none">
              <svg className="w-16 h-16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                <rect x="2" y="3" width="20" height="14" rx="2" />
                <line x1="8" y1="21" x2="16" y2="21" />
                <line x1="12" y1="17" x2="12" y2="21" />
              </svg>
            </div>
            {displayIconPath?.startsWith('live:') && directIconUrl ? (
              <img src={directIconUrl} alt={titleToDisplay} className="absolute inset-0 w-full h-full object-cover z-10" />
            ) : displayIconUrl ? (
              <BlurIcon
                path={displayIconPath}
                pkg={displayIconPkg}
                alt={titleToDisplay}
                priority={true}
                imgClassName="absolute inset-0 w-full h-full object-cover z-10"
              />
            ) : null}
          </div>

          {/* Title Below Package Picture */}
          <h2 className="text-2xl sm:text-3xl font-black text-white mt-6 max-w-2xl truncate">
            {titleToDisplay}
          </h2>

          <p className="text-xs sm:text-sm font-mono text-zinc-400 mt-1">
            {installerStatus.title_id}
            {installerStatus.content_id ? ` • ${installerStatus.content_id}` : ''}
          </p>

          {installerStatus.prompt_message ? (
            <p className="text-xs text-blue-300 font-medium mt-1">
              {installerStatus.prompt_message}
            </p>
          ) : null}

          {/* Wide Progress Bar Across ~3/4 Screen */}
          <div className="w-[75vw] max-w-3xl bg-white/10 rounded-[2px] h-4 sm:h-5 mt-8 overflow-hidden border border-white/20 p-0.5">
            <div
              className="bg-[#0070d1] h-full rounded-[2px] transition-all duration-300"
              style={{ width: `${Math.min(100, Math.max(0, progressVal))}%` }}
            />
          </div>

          {/* Progress Stats & ETA */}
          <div className="w-[75vw] max-w-3xl flex items-center justify-between text-xs sm:text-sm text-zinc-300 mt-3 px-1">
            <span className="font-bold text-white">
              {progressVal.toFixed(1)}%
              <span className="text-zinc-400 font-normal ml-2">
                ({formatBytes(downloadedBytes)} / {formatBytes(totalBytes)})
              </span>
            </span>

            <span>
              {etaInfo && etaInfo.text ? (
                <span>
                  <span className="font-medium text-white">{etaInfo.text}</span>
                  {etaInfo.speedStr ? (
                    <span className="text-zinc-400 font-mono ml-2">({etaInfo.speedStr})</span>
                  ) : null}
                </span>
              ) : (
                <span className="text-zinc-400 font-mono capitalize">{installerStatus.status}</span>
              )}
            </span>
          </div>

          {/* Cancel Installation Button */}
          <div className="mt-8">
            <button
              type="button"
              onClick={onCancel}
              className="px-6 py-2.5 rounded-[2px] ps5-focus-item bg-white/10 hover:bg-rose-600/80 border border-white/20 text-sm font-semibold text-zinc-200 hover:text-white transition-all cursor-pointer"
            >
              Cancel Installation
            </button>
          </div>
        </div>
      </div>
    );
  }

  // ──────────────────────────────────────────────────────────────────────────
  // VIEW C: Full-Screen Refresh
