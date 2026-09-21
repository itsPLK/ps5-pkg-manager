import React from 'react';
import { QRCodeSVG } from 'qrcode.react';
import { DONATE_URL, isPlayStation } from '../../constants/config';
import { formatBytes, formatVersion } from '../../utils/formatters';

export default function SettingsView({ settings, onSaveSettings, onClose, onOpenSmb, onInstallShortcut, installingShortcut, cacheStats, loadingStats, onClearCache, leftoversData, scanningLeftovers, onScanLeftovers, onDeleteLeftover, showDonateQr, setShowDonateQr }) {
  const safeSettings = settings || {};
  const smbSharesCount = Array.isArray(safeSettings.smb_shares)
    ? safeSettings.smb_shares.length
    : (typeof safeSettings.smb_shares === 'number' ? safeSettings.smb_shares : 0);

  return (
<div className="max-w-6xl mx-auto px-4 sm:px-8 space-y-6 pb-12">
            {/* Return Button at top */}
            <div className="flex items-center space-x-4 border-b border-white/10 pb-4">
              <button
                type="button"
                onClick={onClose}
                className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 border border-white/10 text-base font-semibold transition-all flex items-center space-x-2 text-zinc-200 shrink-0 cursor-pointer"
              >
                <span>&larr;</span>
                <span>Back</span>
              </button>

              <div className="text-sm text-zinc-400 flex items-center space-x-2 min-w-0">
                <span className="shrink-0">PKG Manager</span>
                <span className="shrink-0 text-zinc-600">&rsaquo;</span>
                <span className="text-white font-semibold truncate">Settings</span>
              </div>
            </div>

            {/* 2-Column Responsive Layout */}
            <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
              {/* Left Column */}
              <div className="space-y-6">
                {/* Package Organization Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-5">
                  <div className="flex items-center space-x-3 pb-3 border-b border-white/10">
                    <div className="w-10 h-10 rounded-[2px] bg-purple-600/20 border border-purple-500/30 flex items-center justify-center text-purple-400 shrink-0">
                      <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <line x1="8" y1="6" x2="21" y2="6" />
                        <line x1="8" y1="12" x2="21" y2="12" />
                        <line x1="8" y1="18" x2="21" y2="18" />
                        <line x1="3" y1="6" x2="3.01" y2="6" />
                        <line x1="3" y1="12" x2="3.01" y2="12" />
                        <line x1="3" y1="18" x2="3.01" y2="18" />
                      </svg>
                    </div>
                    <div>
                      <h3 className="text-lg font-bold text-white">Library Display</h3>
                      <p className="text-xs text-zinc-400">Display &amp; sorting preferences</p>
                    </div>
                  </div>

                  <button
                    type="button"
                    onClick={() => onSaveSettings({ ...safeSettings, fade_installed_packages: !safeSettings.fade_installed_packages })}
                    className="w-full bg-white/5 hover:bg-white/10 border border-white/10 rounded-[2px] ps5-focus-item p-4 flex items-center justify-between transition-colors cursor-pointer text-left"
                  >
                    <div className="min-w-0 flex-1 mr-4">
                      <span className="text-sm font-semibold text-white block">Fade out installed packages</span>
                      <span className="text-xs text-zinc-400 block mt-1">
                        Dim titles and DLCs already installed on this console.
                      </span>
                    </div>
                    <div className={`w-6 h-6 rounded-[2px] border flex items-center justify-center shrink-0 transition-colors ${
                      safeSettings.fade_installed_packages
                        ? 'bg-blue-600 border-blue-500 text-white'
                        : 'bg-black/40 border-white/20 text-transparent'
                    }`}>
                      <svg className="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    </div>
                  </button>

                  <button
                    type="button"
                    onClick={() => onSaveSettings({ ...safeSettings, move_installed_to_end: !safeSettings.move_installed_to_end })}
                    className="w-full bg-white/5 hover:bg-white/10 border border-white/10 rounded-[2px] ps5-focus-item p-4 flex items-center justify-between transition-colors cursor-pointer text-left"
                  >
                    <div className="min-w-0 flex-1 mr-4">
                      <span className="text-sm font-semibold text-white block">Move installed packages to end</span>
                      <span className="text-xs text-zinc-400 block mt-1">
                        Place fully installed titles at the bottom of the list.
                      </span>
                    </div>
                    <div className={`w-6 h-6 rounded-[2px] border flex items-center justify-center shrink-0 transition-colors ${
                      safeSettings.move_installed_to_end
                        ? 'bg-blue-600 border-blue-500 text-white'
                        : 'bg-black/40 border-white/20 text-transparent'
                    }`}>
                      <svg className="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    </div>
                  </button>

                  <button
                    type="button"
                    onClick={() => onSaveSettings({ ...safeSettings, all_sources_mode: !safeSettings.all_sources_mode })}
                    className="w-full bg-white/5 hover:bg-white/10 border border-white/10 rounded-[2px] ps5-focus-item p-4 flex items-center justify-between transition-colors cursor-pointer text-left"
                  >
                    <div className="min-w-0 flex-1 mr-4">
                      <span className="text-sm font-semibold text-white block">List all packages automatically</span>
                      <span className="text-xs text-zinc-400 block mt-1">
                        Show all packages across all storage sources on launch.
                      </span>
                    </div>
                    <div className={`w-6 h-6 rounded-[2px] border flex items-center justify-center shrink-0 transition-colors ${
                      safeSettings.all_sources_mode
                        ? 'bg-blue-600 border-blue-500 text-white'
                        : 'bg-black/40 border-white/20 text-transparent'
                    }`}>
                      <svg className="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    </div>
                  </button>
                </div>

                {/* Stream Debug Logging Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-5">
                  <div className="flex items-center space-x-3 pb-3 border-b border-white/10">
                    <div className="w-10 h-10 rounded-[2px] bg-amber-600/20 border border-amber-500/30 flex items-center justify-center text-amber-400 shrink-0">
                      <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <polyline points="4 17 10 11 4 5" />
                        <line x1="12" y1="19" x2="20" y2="19" />
                      </svg>
                    </div>
                    <div>
                      <h3 className="text-lg font-bold text-white">Developer</h3>
                      <p className="text-xs text-zinc-400">Debug &amp; diagnostics</p>
                    </div>
                  </div>

                  <button
                    type="button"
                    onClick={() => onSaveSettings({ ...safeSettings, pkg_install_debug: !safeSettings.pkg_install_debug })}
                    className="w-full bg-white/5 hover:bg-white/10 border border-white/10 rounded-[2px] ps5-focus-item p-4 flex items-center justify-between transition-colors cursor-pointer text-left"
                  >
                    <div className="min-w-0 flex-1 mr-4">
                      <span className="text-sm font-semibold text-white block">Package install debug logging</span>
                      <span className="text-xs text-zinc-400 block mt-1">
                        Record every stream server connection and byte-range request to a timestamped log file in /data/pkgmgr/. Use this to capture the exact PS5 download pattern for mock/replay testing.
                      </span>
                    </div>
                    <div className={`w-6 h-6 rounded-[2px] border flex items-center justify-center shrink-0 transition-colors ${
                      safeSettings.pkg_install_debug
                        ? 'bg-amber-600 border-amber-500 text-white'
                        : 'bg-black/40 border-white/20 text-transparent'
                    }`}>
                      <svg className="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                    </div>
                  </button>
                </div>

                {/* Samba (SMB) Shares Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-4">
                  <div className="flex items-center justify-between pb-3 border-b border-white/10">
                    <div className="flex items-center space-x-3">
                      <div className="w-10 h-10 rounded-[2px] bg-cyan-600/20 border border-cyan-500/30 flex items-center justify-center text-cyan-400 shrink-0">
                        <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                          <rect x="2" y="2" width="20" height="8" rx="2" />
                          <rect x="2" y="14" width="20" height="8" rx="2" />
                          <line x1="6" y1="6" x2="6.01" y2="6" />
                          <line x1="6" y1="18" x2="6.01" y2="18" />
                          <path d="M12 10v4" />
                        </svg>
                      </div>
                      <div>
                        <h3 className="text-lg font-bold text-white">Samba (SMB) Shares</h3>
                        <p className="text-xs text-zinc-400">
                          {smbSharesCount} share{smbSharesCount === 1 ? '' : 's'} configured
                        </p>
                      </div>
                    </div>
                  </div>

                  <p className="text-xs text-zinc-300 leading-relaxed">
                    Connect to local network shares (PC or NAS) to browse and install packages remotely.
                  </p>

                  <button
                    type="button"
                    onClick={onOpenSmb}
                    className="w-full py-3 px-4 rounded-[2px] ps5-focus-item bg-cyan-600/20 hover:bg-cyan-600/30 border border-cyan-500/30 text-cyan-200 text-sm font-bold transition-colors flex items-center justify-between cursor-pointer"
                  >
                    <span>Manage Samba Shares</span>
                    <span>&rarr;</span>
                  </button>
                </div>

                {/* Home Screen Shortcut Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-4">
                  <div className="flex items-center justify-between pb-3 border-b border-white/10">
                    <div className="flex items-center space-x-3">
                      <div className="w-10 h-10 rounded-[2px] bg-blue-600/20 border border-blue-500/30 flex items-center justify-center text-blue-400 shrink-0">
                        <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                          <path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
                          <polyline points="9 22 9 12 15 12 15 22" />
                        </svg>
                      </div>
                      <div>
                        <h3 className="text-lg font-bold text-white">Home Screen Shortcut</h3>
                        <p className="text-xs text-zinc-400">Media tab quick launch</p>
                      </div>
                    </div>

                    <button
                      type="button"
                      onClick={onInstallShortcut}
                      disabled={installingShortcut}
                      className="px-4 py-2 rounded-[2px] ps5-focus-item bg-blue-600 hover:bg-blue-500 text-white text-xs font-bold transition-colors cursor-pointer flex items-center space-x-2 disabled:opacity-50"
                    >
                      {installingShortcut ? (
                        <>
                          <div className="ps5-robust-spinner-sm" />
                          <span>Installing...</span>
                        </>
                      ) : (
                        <span>Install Shortcut</span>
                      )}
                    </button>
                  </div>

                  <p className="text-xs text-zinc-300 leading-relaxed">
                    Adds a shortcut to the PS5 Media tab to launch PKG Manager directly from the home screen.
                  </p>
                </div>

                {/* Support & Donations Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-4">
                  <div className="flex items-center space-x-3 pb-3 border-b border-white/10">
                    <div className="w-10 h-10 rounded-[2px] bg-rose-600/20 border border-rose-500/30 flex items-center justify-center text-rose-400 shrink-0">
                      <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                        <path d="M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z" />
                      </svg>
                    </div>
                    <div>
                      <h3 className="text-lg font-bold text-white">Support the Project</h3>
                      <p className="text-xs text-zinc-400">Donations &amp; contributions</p>
                    </div>
                  </div>

                  <p className="text-xs text-zinc-300 leading-relaxed">
                    If you find PKG Manager useful and would like to support its continued development or say thanks, donations are greatly appreciated!
                  </p>

                  {isPlayStation ? (
                    <div className="bg-black/30 border border-white/10 rounded-[2px] p-4 flex flex-col items-center space-y-3">
                      <div className="p-2.5 bg-white rounded-[4px] shadow-lg flex items-center justify-center">
                        <QRCodeSVG
                          value={DONATE_URL}
                          size={144}
                          level="M"
                        />
                      </div>
                      <div className="text-center space-y-1">
                        <span className="text-xs font-semibold text-white block">Scan with your phone</span>
                        <span className="text-[11px] font-mono text-zinc-500 block pt-0.5">
                          github.com/itsPLK/ps5-pkg-manager
                        </span>
                      </div>
                    </div>
                  ) : (
                    <div className="space-y-3">
                      <a
                        href={DONATE_URL}
                        target="_blank"
                        rel="noopener noreferrer"
                        className="w-full py-3 px-4 rounded-[2px] ps5-focus-item bg-rose-600/20 hover:bg-rose-600/30 border border-rose-500/30 text-rose-200 text-sm font-bold transition-colors flex items-center justify-center cursor-pointer"
                      >
                        View donation options
                      </a>
                      <div className="flex justify-center">
                        <button
                          type="button"
                          onClick={() => setShowDonateQr((prev) => !prev)}
                          className="text-[11px] text-zinc-400 hover:text-zinc-200 transition-colors cursor-pointer underline underline-offset-2"
                        >
                          {showDonateQr ? 'Hide QR Code' : 'Show QR Code for phone scan'}
                        </button>
                      </div>
                      {showDonateQr && (
                        <div className="bg-black/30 border border-white/10 rounded-[2px] p-4 flex flex-col items-center space-y-3">
                          <div className="p-2.5 bg-white rounded-[4px] shadow-lg flex items-center justify-center">
                            <QRCodeSVG
                              value={DONATE_URL}
                              size={144}
                              level="M"
                            />
                          </div>
                          <div className="text-center space-y-1">
                            <span className="text-xs font-semibold text-white block">Scan with your phone</span>
                            <span className="text-[11px] font-mono text-zinc-500 block pt-0.5">
                              github.com/itsPLK/ps5-pkg-manager
                            </span>
                          </div>
                        </div>
                      )}
                    </div>
                  )}
                </div>
              </div>

              {/* Right Column */}
              <div className="space-y-6">
                {/* Package Cache Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-4">
                  <div className="flex items-center justify-between pb-3 border-b border-white/10">
                    <div className="flex items-center space-x-3">
                      <div className="w-10 h-10 rounded-[2px] bg-emerald-600/20 border border-emerald-500/30 flex items-center justify-center text-emerald-400 shrink-0">
                        <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                          <rect x="2" y="2" width="20" height="8" rx="2" ry="2" />
                          <rect x="2" y="14" width="20" height="8" rx="2" ry="2" />
                          <line x1="6" y1="6" x2="6.01" y2="6" />
                          <line x1="6" y1="18" x2="6.01" y2="18" />
                        </svg>
                      </div>
                      <div>
                        <h3 className="text-lg font-bold text-white">Cache</h3>
                        <p className="text-xs text-zinc-400">Metadata &amp; icons</p>
                      </div>
                    </div>

                    {cacheStats && cacheStats.total_count > 0 && (
                      <button
                        type="button"
                        onClick={onClearCache}
                        className="px-4 py-2 rounded-[2px] ps5-focus-item bg-rose-600/20 hover:bg-rose-600/30 border border-rose-500/30 text-rose-300 text-xs font-bold transition-colors cursor-pointer"
                      >
                        Clear Cache...
                      </button>
                    )}
                  </div>

                  {/* Cache Overview Banner */}
                  <div className="bg-black/30 border border-white/5 rounded-[2px] p-4 flex items-center justify-between">
                    <div>
                      <span className="text-xs text-zinc-400 uppercase tracking-wider font-semibold block">Cache Size</span>
                      <span className="text-xl font-bold text-white font-mono mt-0.5 block">
                        {loadingStats ? 'Checking...' : formatBytes((cacheStats && cacheStats.total_bytes) || 0)}
                      </span>
                    </div>
                    <div className="text-right">
                      <span className="text-xs text-zinc-400 uppercase tracking-wider font-semibold block">Cached Packages</span>
                      <span className="text-xl font-bold text-blue-400 font-mono mt-0.5 block">
                        {loadingStats ? '...' : ((cacheStats && cacheStats.total_count) || 0)}
                      </span>
                    </div>
                  </div>

                  {/* Cache Directory Location */}
                  <div className="bg-white/5 border border-white/10 rounded-[2px] p-3 flex items-center justify-between">
                    <div className="min-w-0 flex-1">
                      <span className="text-xs font-semibold text-zinc-400 block">Location</span>
                      <span className="text-xs font-mono text-zinc-200 block truncate mt-0.5">
                        {(cacheStats && cacheStats.cache_path) || '/data/pkgmgr/cache'}
                      </span>
                    </div>
                  </div>
                </div>

                {/* Orphaned Leftovers Cleanup Card */}
                <div className="rounded-[2px] bg-[#141520] border border-white/10 p-6 space-y-4">
                  <div className="flex items-center justify-between pb-3 border-b border-white/10">
                    <div className="flex items-center space-x-3">
                      <div className="w-10 h-10 rounded-[2px] bg-amber-600/20 border border-amber-500/30 flex items-center justify-center text-amber-400 shrink-0">
                        <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                          <path strokeLinecap="round" strokeLinejoin="round" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
                        </svg>
                      </div>
                      <div>
                        <h3 className="text-lg font-bold text-white">Leftover Cleanup</h3>
                        <p className="text-xs text-zinc-400">Orphaned updates &amp; DLCs</p>
                      </div>
                    </div>

                    <button
                      type="button"
                      onClick={onScanLeftovers}
                      disabled={scanningLeftovers}
                      className="px-4 py-2 rounded-[2px] ps5-focus-item bg-amber-600/20 hover:bg-amber-600/30 border border-amber-500/30 text-amber-300 text-xs font-bold transition-colors cursor-pointer disabled:opacity-50 flex items-center space-x-2 shrink-0"
                    >
                      {scanningLeftovers ? (
                        <>
                          <div className="ps5-robust-spinner-sm" />
                          <span>Scanning...</span>
                        </>
                      ) : (
                        <span>{leftoversData ? 'Rescan' : 'Scan'}</span>
                      )}
                    </button>
                  </div>

                  {/* Body Content */}
                  {(!leftoversData && !scanningLeftovers) ? (
                    <div className="bg-black/30 border border-white/5 rounded-[2px] p-4 text-center">
                      <p className="text-xs text-zinc-400 leading-relaxed">
                        Scan console storage for leftover updates or DLCs from uninstalled base packages.
                      </p>
                    </div>
                  ) : scanningLeftovers ? (
                    <div className="py-8 text-center space-y-3">
                      <div className="ps5-robust-spinner mx-auto" />
                      <p className="text-xs text-zinc-400">Scanning for leftover files...</p>
                    </div>
                  ) : (leftoversData && leftoversData.count === 0) ? (
                    <div className="bg-emerald-500/10 border border-emerald-500/20 rounded-[2px] p-4 flex items-center space-x-3 text-emerald-300">
                      <svg className="w-5 h-5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
                        <polyline points="20 6 9 17 4 12" />
                      </svg>
                      <span className="text-xs font-medium">Console storage is clean. No orphaned packages found.</span>
                    </div>
                  ) : (leftoversData && leftoversData.leftovers) ? (
                    <div className="space-y-3">
                      <div className="flex items-center justify-between text-xs text-zinc-400 px-1">
                        <span>
                          Found <strong className="text-white">{leftoversData.count || 0}</strong> orphaned {(leftoversData.count === 1) ? 'package' : 'packages'}
                        </span>
                        <span>
                          Total:{' '}
                          <strong className="text-amber-400 font-mono">
                            {formatBytes((leftoversData.leftovers || []).reduce((acc, x) => acc + (x.total_size || 0), 0))}
                          </strong>
                        </span>
                      </div>

                      <div className="space-y-2.5 max-h-72 overflow-y-auto pr-1">
                        {(leftoversData.leftovers || []).map((item) => (
                          <div
                            key={item.title_id}
                            className="bg-white/5 border border-white/10 rounded-[2px] p-3.5 flex items-center justify-between space-x-3 hover:border-white/20 transition-colors"
                          >
                            <div className="flex items-center space-x-3 min-w-0 flex-1">
                              <div className="w-10 h-10 rounded-[2px] bg-amber-500/10 border border-amber-500/20 shrink-0 flex items-center justify-center text-amber-400">
                                <svg className="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.75">
                                  <path strokeLinecap="round" strokeLinejoin="round" d="M14.74 9l-.346 9m-4.788 0L9.26 9m9.968-3.21c.342.052.682.107 1.022.166m-1.022-.165L18.16 19.673a2.25 2.25 0 01-2.244 2.077H8.084a2.25 2.25 0 01-2.244-2.077L4.772 5.79m14.456 0a48.108 48.108 0 00-3.478-.397m-12 .562c.34-.059.68-.114 1.022-.165m0 0a48.11 48.11 0 013.478-.397m7.5 0v-.916c0-1.18-.91-2.164-2.09-2.201a51.964 51.964 0 00-3.32 0c-1.18.037-2.09 1.022-2.09 2.201v.916m7.5 0a48.667 48.667 0 00-7.5 0" />
                                </svg>
                              </div>

                              <div className="min-w-0 flex-1">
                                <div className="flex items-center space-x-2">
                                  <span className="text-sm font-bold text-white truncate">{item.title_name}</span>
                                  <span className="text-[10px] font-mono px-1.5 py-0.5 rounded-[2px] bg-black/40 text-zinc-400 border border-white/10 shrink-0">
                                    {item.title_id}
                                  </span>
                                </div>
                                <div className="flex items-center space-x-2 mt-1 flex-wrap">
                                  <span className={`text-[10px] font-bold px-1.5 py-0.5 rounded-[2px] border ${
                                    item.type === 'Orphaned Update'
                                      ? 'bg-purple-950/70 text-purple-300 border-purple-800/40'
                                      : item.type === 'Orphaned DLC'
                                      ? 'bg-emerald-950/70 text-emerald-300 border-emerald-800/40'
                                      : 'bg-amber-950/70 text-amber-300 border-amber-800/40'
                                  }`}>
                                    {item.type} {item.version ? `(${formatVersion(item.version)})` : ''}
                                  </span>
                                  <span className="text-xs font-mono text-zinc-300 font-bold">
                                    {formatBytes(item.total_size)}
                                  </span>
                                  <span className="text-[11px] text-zinc-500">
                                    ({Array.isArray(item.paths) ? item.paths.length : (item.paths || 0)} locations)
                                  </span>
                                </div>
                              </div>
                            </div>

                            <button
                              type="button"
                              onClick={() => onDeleteLeftover(item)}
                              className="px-3 py-1.5 rounded-[2px] ps5-focus-item bg-rose-600/20 hover:bg-rose-600/30 text-rose-300 border border-rose-500/30 text-xs font-bold transition-colors cursor-pointer shrink-0"
                            >
                              Delete...
                            </button>
                          </div>
                        ))}
                      </div>
                    </div>
                  ) : null}
                </div>
              </div>
            </div>
    </div>
  );
}
