import React from 'react';

export const TONES = {
  muted: 'bg-white/5 text-zinc-300 border-white/15',
  blue: 'bg-[#0095ff]/15 text-[#5cb8ff] border-[#0095ff]/40',
  green: 'bg-green-500/15 text-green-400 border-green-500/40',
  amber: 'bg-amber-500/15 text-amber-300 border-amber-500/40',
  red: 'bg-red-500/15 text-red-300 border-red-500/40',
};

const BUTTON = 'px-3 py-1.5 rounded border text-xs font-semibold cursor-pointer disabled:opacity-40 disabled:cursor-not-allowed ';
export const toolButton = BUTTON + 'bg-white/10 hover:bg-white/15 border-white/10 text-zinc-200';
export const primaryButton = BUTTON + 'bg-green-600 hover:bg-green-500 border-transparent text-white';
export const dangerButton = BUTTON + 'bg-red-600 hover:bg-red-500 border-transparent text-white';
export const panelButton = BUTTON + 'bg-white/10 border-white/15 text-zinc-200 hover:text-white';
export const roundButton = 'w-8 h-8 rounded-full border border-white/20 hover:border-[#0095ff] hover:text-[#5cb8ff] text-zinc-300 flex items-center justify-center cursor-pointer disabled:opacity-30 disabled:cursor-not-allowed disabled:hover:border-white/20 disabled:hover:text-zinc-300';

export function Chip({ tone = 'muted', title, children }) {
  return (
    <span title={title || ''} className={'inline-block px-2 py-0.5 rounded border text-xs whitespace-nowrap ' + TONES[tone]}>
      {children}
    </span>
  );
}

export function Chevron({ open, onClick, label }) {
  return (
    <button type="button" onClick={onClick} aria-expanded={open} aria-label={label}
      className="text-zinc-500 hover:text-zinc-200 cursor-pointer px-2">
      <span className={'inline-block transition-transform ' + (open ? 'rotate-90' : '')}>›</span>
    </button>
  );
}

export function Icon({ url, size = 'w-9 h-9' }) {
  return (
    <div className={size + ' shrink-0 rounded bg-black/30 border border-white/10 overflow-hidden flex items-center justify-center'}>
      {url ? <img src={url} alt="" className="w-full h-full object-cover" /> : <span className="text-[10px] text-zinc-500">PKG</span>}
    </div>
  );
}

export function ProgressBar({ percent, height = 'h-2.5', color = 'bg-[#0070d1]' }) {
  return (
    <div className={'w-full bg-white/10 rounded overflow-hidden ' + height}>
      <div className={'h-full transition-all duration-300 ' + color} style={{ width: Math.min(100, Math.max(0, percent)) + '%' }} />
    </div>
  );
}

const BANNER_TONES = {
  amber: 'border-amber-500/30 bg-amber-500/10 text-amber-200',
  green: 'border-green-500/30 bg-green-500/10 text-green-200',
  red: 'border-red-500/30 bg-red-500/10 text-red-200',
};

export function Banner({ tone = 'amber', children, onDismiss }) {
  return (
    <div className={'flex items-start justify-between gap-3 px-3 py-2 rounded border text-xs ' + BANNER_TONES[tone]}>
      <div className="min-w-0">{children}</div>
      {onDismiss && <button type="button" onClick={onDismiss} className="shrink-0 hover:text-white cursor-pointer">Dismiss</button>}
    </div>
  );
}

// Title, IDs and an optional kicker line shared by the install panels.
export function InstallHeading({ kicker, kickerClass, title, ids }) {
  return (
    <div className="min-w-0">
      <p className={'text-[11px] uppercase tracking-wider font-bold ' + kickerClass}>{kicker}</p>
      <p className="truncate font-semibold text-white">{title}</p>
      <p className="truncate text-xs font-mono text-zinc-400">{ids.filter(Boolean).join(' • ')}</p>
    </div>
  );
}

export const stopPropagation = (handler) => (event) => { event.stopPropagation(); handler(); };
