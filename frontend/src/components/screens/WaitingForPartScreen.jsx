import React from 'react';


export default function WaitingForPartScreen({ installerStatus, isDiscSource, onCancel }) {
const discTitle = isDiscSource
      ? (installerStatus.prompt_message || `Please Insert Disc ${installerStatus.current_part}`)
      : (installerStatus.prompt_message || `Waiting for Part ${installerStatus.current_part}`);

    const discDesc = isDiscSource
      ? `Disc ${installerStatus.current_part - 1} was copied. Please eject the disc, insert Disc ${installerStatus.current_part} into the PS5 drive, and wait. The installer will automatically detect it and resume copying.`
      : `Waiting for package part ${installerStatus.current_part} of ${installerStatus.total_parts}. Please connect the USB drive containing Part ${installerStatus.current_part}. The installer will automatically detect the file and continue.`;

    const statusBadge = isDiscSource ? 'Scanning /mnt/disc...' : `Waiting for Part ${installerStatus.current_part}...`;

    return (
      <div className="fixed inset-0 z-50 bg-[#0a0a0f] text-white flex flex-col items-center justify-center p-6 overflow-hidden select-none">
        <div className="relative z-10 flex flex-col items-center max-w-2xl w-full text-center">
          {/* Big Icon */}
          <div className="w-28 h-28 rounded-[2px] bg-[#141520] border border-amber-500/40 flex items-center justify-center text-amber-400 mb-6">
            {isDiscSource ? (
              <svg className="w-16 h-16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75">
                <circle cx="12" cy="12" r="10" />
                <circle cx="12" cy="12" r="3" />
                <line x1="12" y1="2" x2="12" y2="5" />
                <line x1="12" y1="19" x2="12" y2="22" />
              </svg>
            ) : (
              <svg className="w-16 h-16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
                <path d="M9 2h6v5H9z" />
                <rect x="10.5" y="3.5" width="1" height="1.5" fill="currentColor" stroke="none" />
                <rect x="12.5" y="3.5" width="1" height="1.5" fill="currentColor" stroke="none" />
                <rect x="6" y="7" width="12" height="14" rx="2" />
                <line x1="9" y1="11" x2="15" y2="11" />
              </svg>
            )}
          </div>

          <div className="inline-block px-3 py-1 rounded-[2px] bg-amber-500/20 text-amber-300 border border-amber-500/30 text-xs uppercase font-bold tracking-wider mb-3">
            {isDiscSource
              ? `Disc ${installerStatus.current_part} of ${installerStatus.total_parts}`
              : `Part ${installerStatus.current_part} of ${installerStatus.total_parts}`}
          </div>

          {installerStatus.title_name ? (
            <p className="text-sm font-bold text-zinc-300 mb-1 truncate max-w-lg">
              {installerStatus.title_name}
            </p>
          ) : null}

          <h2 className="text-3xl font-black text-white">{discTitle}</h2>
          <p className="text-sm text-zinc-300 mt-3 max-w-lg leading-relaxed">{discDesc}</p>

          <div className="mt-6 flex items-center space-x-2 bg-black/40 px-4 py-2 rounded-[2px] border border-white/10 text-xs font-mono text-zinc-400">
            <span className="w-2 h-2 rounded-full bg-amber-400" />
            <span>{statusBadge}</span>
          </div>

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
