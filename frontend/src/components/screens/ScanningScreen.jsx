import React from 'react';


export default function ScanningScreen({ scanStatus }) {
const totalFiles = scanStatus.total_files || 0;
    const processedFiles = scanStatus.processed_files || 0;
    const currentFile = scanStatus.current_file || '';
    const currentDrive = scanStatus.current_drive || '';
    const hasCounts = totalFiles > 0;
    const percent = hasCounts ? Math.min(100, Math.round((processedFiles / totalFiles) * 100)) : null;

    return (
      <div className="fixed inset-0 z-50 bg-[#0a0a0f] text-white flex flex-col items-center justify-center p-6 select-none">
        <div className="flex flex-col items-center max-w-md w-full text-center space-y-6">
          <div className="ps5-robust-spinner" />

          <div>
            <h2 className="text-xl font-bold text-white">
              Scanning Storage Media...
            </h2>
            <p className="text-xs text-zinc-400 mt-1 truncate">
              {currentDrive || 'Scanning connected drives...'}
            </p>
          </div>

          {/* Simple solid progress bar */}
          <div className="w-full bg-[#141520] border border-white/10 rounded-[2px] p-4 space-y-2.5">
            <div className="flex items-center justify-between text-xs font-mono">
              <span className="text-zinc-400 truncate max-w-[70%]">
                {currentFile ? currentFile : 'Indexing packages...'}
              </span>
              <span className="text-white font-bold">
                {percent !== null ? `${percent}%` : ''}
              </span>
            </div>

            <div className="w-full bg-black/60 h-2 rounded-[2px] overflow-hidden border border-white/10">
              <div
                className="bg-blue-600 h-full rounded-[2px]"
                style={{ width: `${percent !== null ? percent : 0}%` }}
              />
            </div>

            {hasCounts ? (
              <div className="text-right text-[11px] font-mono text-zinc-500">
                {processedFiles} / {totalFiles} pkgs
              </div>
            ) : null}
          </div>
        </div>
      </div>
    );
  }
