import React from 'react';


export default function LoadingScreen() {
return (
      <div className="min-h-screen bg-[#0a0a0f] text-white flex items-center justify-center">
        <div className="text-center">
          <div className="ps5-robust-spinner mx-auto" />
          <p className="text-xs text-zinc-400 mt-3 font-mono">Connecting to PKG Manager...</p>
        </div>
      </div>
    );
  }
