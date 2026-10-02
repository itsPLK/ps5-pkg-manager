import React from 'react';
import { getFullVersion } from '../../utils/title';
import { isPlayStation } from '../../constants/config';

export default function Footer({ appVersion }) {
  return (
    <footer className="w-full py-4 px-4 text-center text-[11px] text-zinc-600 border-t border-white/5 select-none">
      {/* Footer */}
        <div className="flex flex-wrap items-center justify-center gap-x-3 gap-y-1">
          <span>{getFullVersion(appVersion)}</span>
          <span className="text-zinc-700 hidden sm:inline">&bull;</span>
          {isPlayStation ? (
            <span>This project is free and open source: github.com/itsPLK/ps5-pkg-manager</span>
          ) : (
            <span>
              This project is free and open source:{' '}
              <a
                href="https://github.com/itsPLK/ps5-pkg-manager"
                target="_blank"
                rel="noopener noreferrer"
                className="text-zinc-500 hover:text-zinc-400 underline underline-offset-2 transition-colors"
              >
                GitHub
              </a>
            </span>
          )}
        </div>
      </footer>
  );
}
