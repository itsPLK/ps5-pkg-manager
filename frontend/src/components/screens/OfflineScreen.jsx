import React from 'react';


export default function OfflineScreen({ onRetry }) {
return (
      <div className="min-h-screen bg-[#0a0a0f] text-zinc-100 font-ps5 flex flex-col items-center justify-center p-4 text-center select-none">
        <div className="max-w-lg p-12 bg-black/40 rounded-3xl border border-white/5 backdrop-blur-sm shadow-2xl">
          <div className="text-7xl font-light text-zinc-400 mb-8 font-mono">:(</div>
          <h1 className="text-2xl font-bold mb-4 text-zinc-200">Internal PKG Manager is not running...</h1>
          <p className="text-lg text-zinc-400 leading-relaxed mb-6">
            Please ensure you have loaded <strong className="text-white font-mono">pkgmgr.elf</strong> on your PS5 before launching this application.
          </p>
          <button
            onClick={onRetry}
            className="px-6 py-2.5 bg-[#0070d1] hover:bg-[#0095ff] text-white font-medium rounded-lg transition-colors cursor-pointer"
          >
            Retry Connection
          </button>
        </div>
      </div>
    );
}
