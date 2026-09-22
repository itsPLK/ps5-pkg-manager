import React from 'react';


export default function SmbManagementView({ settings, onBack, onAdd, onEdit, onToggle, onRemove, onTest, testing }) {
  const safeSettings = settings || {};
  const sharesList = Array.isArray(safeSettings.smb_shares) ? safeSettings.smb_shares : [];
  const totalShares = sharesList.length;
  const enabledShares = sharesList.filter((s) => s && s.enabled).length;

  return (
<div className="max-w-6xl mx-auto px-4 sm:px-8 space-y-6 pb-12">
            {/* Return Button & Breadcrumbs at top */}
            <div className="flex items-center justify-between border-b border-white/10 pb-4">
              <div className="flex items-center space-x-4 min-w-0 flex-1">
                <button
                  type="button"
                  onClick={onBack}
                  className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 border border-white/10 text-base font-semibold transition-all flex items-center space-x-2 text-zinc-200 shrink-0 cursor-pointer"
                >
                  <span>&larr;</span>
                  <span>Back to Settings</span>
                </button>

                <div className="text-sm text-zinc-400 flex items-center space-x-2 min-w-0">
                  <span className="shrink-0">PKG Manager</span>
                  <span className="shrink-0 text-zinc-600">&rsaquo;</span>
                  <button
                    type="button"
                    onClick={onBack}
                    className="shrink-0 text-zinc-400 hover:text-white transition-colors cursor-pointer"
                  >
                    Settings
                  </button>
                  <span className="shrink-0 text-zinc-600">&rsaquo;</span>
                  <span className="text-white font-semibold truncate">Samba Shares</span>
                </div>
              </div>

              <button
                type="button"
                onClick={onAdd}
                className="px-4 py-2.5 rounded-[2px] ps5-focus-item bg-cyan-600 hover:bg-cyan-500 text-white text-sm font-bold transition-colors flex items-center space-x-1.5 cursor-pointer shrink-0"
              >
                <span>+ Add Share</span>
              </button>
            </div>

            {/* Shares Header Card */}
            <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 flex flex-col sm:flex-row items-start sm:items-center justify-between gap-4">
              <div className="flex items-center space-x-4">
                <div className="w-12 h-12 rounded-[2px] bg-cyan-600/20 border border-cyan-500/30 flex items-center justify-center text-cyan-400 shrink-0">
                  <svg className="w-6 h-6" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                    <rect x="2" y="2" width="20" height="8" rx="2" />
                    <rect x="2" y="14" width="20" height="8" rx="2" />
                    <line x1="6" y1="6" x2="6.01" y2="6" />
                    <line x1="6" y1="18" x2="6.01" y2="18" />
                    <path d="M12 10v4" />
                  </svg>
                </div>
                <div>
                  <h2 className="text-xl font-bold text-white">Samba (SMB) Shares</h2>
                  <p className="text-xs text-zinc-400 mt-0.5">
                    Browse and install packages over local network
                  </p>
                </div>
              </div>

              <div className="flex items-center space-x-2 text-xs font-mono">
                <span className="px-3 py-1 rounded-[2px] bg-white/5 border border-white/10 text-zinc-300">
                  {totalShares} {totalShares === 1 ? 'Share' : 'Shares'}
                </span>
                <span className="px-3 py-1 rounded-[2px] bg-emerald-500/15 text-emerald-300 border border-emerald-500/30">
                  {enabledShares} Enabled
                </span>
              </div>
            </div>

            {/* Shares List */}
            {sharesList.length === 0 ? (
              <div className="py-16 text-center rounded-[2px] border border-white/10 bg-[#12131a]/40 p-8 space-y-4">
                <div className="w-16 h-16 rounded-[2px] bg-white/5 border border-white/10 mx-auto flex items-center justify-center text-zinc-500">
                  <svg className="w-8 h-8" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                    <rect x="2" y="2" width="20" height="8" rx="2" />
                    <rect x="2" y="14" width="20" height="8" rx="2" />
                    <line x1="6" y1="6" x2="6.01" y2="6" />
                    <line x1="6" y1="18" x2="6.01" y2="18" />
                    <path d="M12 10v4" />
                  </svg>
                </div>
                <h3 className="text-lg font-bold text-white">No Samba Shares Configured</h3>
                <p className="text-xs text-zinc-400 max-w-md mx-auto leading-relaxed">
                  Connect to a Windows PC or NAS Samba share to install packages directly over your local network.
                </p>
                <button
                  type="button"
                  onClick={onAdd}
                  className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-cyan-600 hover:bg-cyan-500 text-white font-bold text-xs transition-colors cursor-pointer"
                >
                  + Add Samba Share
                </button>
              </div>
            ) : (
              <div className="space-y-4">
                {sharesList.map((sh, idx) => {
                  const portStr = (sh.port && sh.port !== 445) ? `:${sh.port}` : '';
                  const pathStr = sh.path ? `/${sh.path.replace(/^\/+/, '')}` : '';
                  const urlStr = `smb://${sh.server}${portStr}/${sh.share}${pathStr}`;
                  return (
                    <div
                      key={sh.id || idx}
                      className={`rounded-[2px] p-5 border transition-all ${
                        sh.enabled ? 'bg-[#141520] border-white/10' : 'bg-[#12131b]/60 border-white/5 opacity-50'
                      }`}
                    >
                      <div className="flex flex-col md:flex-row md:items-center justify-between gap-4">
                        <div className="min-w-0 flex-1">
                          <div className="flex items-center space-x-3 flex-wrap gap-y-1">
                            <h4 className="text-base font-bold text-white truncate">
                              {sh.label || `${sh.server}/${sh.share}`}
                            </h4>
                            {!sh.enabled && (
                              <span className="text-[10px] px-2 py-0.5 rounded-[2px] font-semibold bg-zinc-800 text-zinc-400 border border-zinc-700 shrink-0">
                                Disabled
                              </span>
                            )}
                            {sh.username && (
                              <span className="text-[10px] px-2 py-0.5 rounded-[2px] font-mono bg-white/5 text-zinc-400 border border-white/10 shrink-0">
                                user: {sh.username}
                              </span>
                            )}
                          </div>
                          <p className="text-xs font-mono text-cyan-300/80 mt-1.5 truncate select-all">{urlStr}</p>
                        </div>

                        {/* Actions */}
                        <div className="flex items-center space-x-2 shrink-0">
                          <button
                            type="button"
                            onClick={() => onTest(sh)}
                            disabled={testing}
                            className="px-3 py-2 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 text-zinc-200 hover:text-white border border-white/10 text-xs font-semibold transition-colors cursor-pointer flex items-center space-x-1.5"
                          >
                            <span>Test</span>
                          </button>
                          <button
                            type="button"
                            onClick={() => onToggle(idx)}
                            className={`px-3 py-2 rounded-[2px] ps5-focus-item text-xs font-semibold border transition-colors cursor-pointer ${
                              sh.enabled
                                ? 'bg-white/5 text-zinc-300 hover:bg-white/10 border-white/10'
                                : 'bg-cyan-600/20 text-cyan-300 border-cyan-500/30 hover:bg-cyan-600/30'
                            }`}
                          >
                            {sh.enabled ? 'Disable' : 'Enable'}
                          </button>
                          <button
                            type="button"
                            onClick={() => onEdit(idx, sh)}
                            className="px-3 py-2 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 text-zinc-200 hover:text-white border border-white/10 text-xs font-semibold transition-colors cursor-pointer"
                          >
                            Edit
                          </button>
                          <button
                            type="button"
                            onClick={() => onRemove(idx)}
                            className="px-3 py-2 rounded-[2px] ps5-focus-item bg-rose-600/15 hover:bg-rose-600/25 text-rose-300 border border-rose-500/25 text-xs font-semibold transition-colors cursor-pointer"
                          >
                            Delete
                          </button>
                        </div>
                      </div>
                    </div>
                  );
                })}
              </div>
            )}
          </div>
  );
}
