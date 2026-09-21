import React from 'react';
import { formatBytes } from '../../utils/formatters';

const ALL_SOURCES_DRIVE = {
  id: '__all__',
  label: 'All Sources',
  path: 'Combined storage',
  type: 'all',
  clickable: true
};

export default function DrivesView({ drives, storage, onSelectDrive, onDirectInstall, showDirectInstall, loadingDrives, refreshAll, onRescan }) {
  const handleSelectDrive = (drive) => {
    if (onSelectDrive) onSelectDrive(drive);
  };
  const handleRescan = onRescan || refreshAll || function() {};
  
  return (
          <div className="space-y-4">
            <div className="flex items-center space-x-3">
              <h2 className="text-xl font-bold text-white">Select Storage Media</h2>
              <span className="text-xs px-2.5 py-0.5 rounded-[2px] bg-white/5 border border-white/10 text-zinc-400">
                {drives.length} Detected
              </span>
            </div>

            {loadingDrives ? (
              <div className="py-24 text-center">
                <div className="ps5-robust-spinner mx-auto" />
                <p className="text-sm text-zinc-400 mt-3">Scanning mounted drives...</p>
              </div>
            ) : drives.length === 0 && !showDirectInstall ? (
              <div className="py-20 text-center rounded-[2px] border border-white/10 bg-[#12131a]/40 p-8">
                <div className="w-20 h-20 rounded-[2px] bg-white/5 border border-white/10 mx-auto flex items-center justify-center text-zinc-500 mb-4">
                  <svg className="w-10 h-10" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                    <rect x="2" y="6" width="20" height="12" rx="2" />
                    <circle cx="12" cy="12" r="3" />
                    <path d="M6 12h.01M18 12h.01" />
                  </svg>
                </div>
                <h3 className="text-xl font-bold text-white">No Storage Media Found</h3>
                <p className="text-sm text-zinc-400 max-w-md mx-auto mt-2">
                  No mounted USB drives (/mnt/usb0-7) or Blu-ray discs (/mnt/disc) were detected. Insert a drive with <span className="font-mono text-zinc-200">.pkg</span> files and click Rescan.
                </p>
                <button
                  type="button"
                  onClick={handleRescan}
                  className="mt-6 px-6 py-3 rounded-[2px] ps5-focus-item bg-blue-600 hover:bg-blue-500 text-white font-semibold text-sm transition-colors"
                >
                  Rescan Drives
                </button>
              </div>
            ) : (
              <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-4">
                {showDirectInstall && (
                  <button type="button" onClick={onDirectInstall}
                    className="group relative rounded-[2px] ps5-focus-item p-5 transition-all flex items-center space-x-4 border text-left bg-[#141520] border-white/10 hover:border-white/20 hover:bg-[#171824] cursor-pointer">
                    <div className="w-14 h-14 rounded-[2px] flex items-center justify-center border shrink-0 bg-green-950/40 border-green-500/40 text-green-300">
                      <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75">
                        <path d="M12 3v12M7 10l5 5 5-5M4 21h16" />
                      </svg>
                    </div>
                    <div className="min-w-0 flex-1">
                      <h3 className="text-base font-bold text-white">Direct Install</h3>
                      <p className="text-xs font-mono text-zinc-400 mt-0.5 truncate">Stream a .pkg from this computer</p>
                    </div>
                  </button>
                )}
                {/* All Sources Card */}
                {drives.length > 0 && <button
                  type="button"
                  onClick={() => handleSelectDrive(ALL_SOURCES_DRIVE)}
                  className="group relative rounded-[2px] ps5-focus-item p-5 transition-all flex items-center space-x-4 border text-left bg-[#141520] border-white/10 hover:border-white/20 hover:bg-[#171824] cursor-pointer"
                >
                  <div className="w-14 h-14 rounded-[2px] flex items-center justify-center border shrink-0 bg-indigo-950/40 border-indigo-500/40 text-indigo-300">
                    <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
                      <rect x="2" y="2" width="20" height="8" rx="2" />
                      <rect x="2" y="14" width="20" height="8" rx="2" />
                      <line x1="6" y1="6" x2="6.01" y2="6" />
                      <line x1="6" y1="18" x2="6.01" y2="18" />
                      <path d="M12 10v4" />
                    </svg>
                  </div>

                  <div className="min-w-0 flex-1">
                    <div className="flex items-center space-x-2">
                      <h3 className="text-base font-bold text-white transition-colors truncate">
                        All Sources
                      </h3>
                      <span className="px-2 py-0.5 rounded-[2px] text-[11px] font-bold border shrink-0 bg-indigo-500/20 text-indigo-300 border-indigo-500/30">
                        Combined
                      </span>
                    </div>
                    <p className="text-xs font-mono text-zinc-400 mt-0.5 truncate">
                      All connected storage &amp; shares
                    </p>
                  </div>
                </button>}

                {drives.map((d) => {
                  const isUsb = d.type === 'usb';
                  const isDisc = d.type === 'disc';
                  const isSmb = d.type === 'smb';
                  const isClickable = d.clickable;

                  return (
                    <button
                      key={d.id}
                      type="button"
                      disabled={!isClickable}
                      onClick={() => isClickable && handleSelectDrive(d)}
                      className={`group relative rounded-[2px] p-5 transition-all flex items-center space-x-4 border text-left ${
                        isClickable
                          ? 'ps5-focus-item bg-[#141520] border-white/10 hover:border-white/20 hover:bg-[#171824] cursor-pointer'
                          : 'bg-[#12131b]/60 border-white/5 opacity-40 cursor-not-allowed'
                      }`}
                    >
                      <div
                        className={`w-14 h-14 rounded-[2px] flex items-center justify-center border shrink-0 ${
                          isClickable
                            ? isDisc
                              ? 'bg-purple-950/40 border-purple-500/40 text-purple-300'
                              : isSmb
                              ? 'bg-cyan-950/40 border-cyan-500/40 text-cyan-300'
                              : 'bg-blue-950/40 border-blue-500/40 text-blue-300'
                            : 'bg-white/5 border-white/5 text-zinc-600'
                        }`}
                      >
                        {isDisc ? (
                          /* Blu-ray Disc Icon */
                          <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75">
                            <circle cx="12" cy="12" r="10" />
                            <circle cx="12" cy="12" r="3" />
                            <line x1="12" y1="2" x2="12" y2="5" />
                            <line x1="12" y1="19" x2="12" y2="22" />
                          </svg>
                        ) : isSmb ? (
                          /* Samba Network Share Icon */
                          <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
                            <rect x="2" y="2" width="20" height="8" rx="2" />
                            <rect x="2" y="14" width="20" height="8" rx="2" />
                            <line x1="6" y1="6" x2="6.01" y2="6" />
                            <line x1="6" y1="18" x2="6.01" y2="18" />
                            <path d="M12 10v4" />
                          </svg>
                        ) : (
                          /* USB Drive Icon */
                          <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
                            <path d="M9 2h6v5H9z" />
                            <rect x="10.5" y="3.5" width="1" height="1.5" fill="currentColor" stroke="none" />
                            <rect x="12.5" y="3.5" width="1" height="1.5" fill="currentColor" stroke="none" />
                            <rect x="6" y="7" width="12" height="14" rx="2" />
                            <line x1="9" y1="11" x2="15" y2="11" />
                          </svg>
                        )}
                      </div>

                      <div className="min-w-0 flex-1">
                        <div className="flex items-center space-x-2">
                          <h3 className="text-base font-bold text-white transition-colors truncate">
                            {d.label}
                          </h3>
                          <span
                            className={`px-2 py-0.5 rounded-[2px] text-[11px] font-bold border shrink-0 ${
                              isClickable
                                ? isSmb
                                  ? 'bg-cyan-500/20 text-cyan-300 border-cyan-500/30'
                                  : isDisc
                                  ? 'bg-purple-500/20 text-purple-300 border-purple-500/30'
                                  : 'bg-blue-500/20 text-blue-300 border-blue-500/30'
                                : 'bg-white/5 text-zinc-500 border-white/10'
                            }`}
                          >
                            {d.pkg_count} {d.pkg_count === 1 ? 'PKG' : 'PKGs'}
                          </span>
                        </div>
                        <p className="text-xs font-mono text-zinc-400 mt-0.5 truncate">
                          {d.path}
                        </p>
                      </div>
                    </button>
                  );
                })}
              </div>
            )}
          </div>
  );
}
