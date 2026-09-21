import React from 'react';
import BlurIcon from '../../BlurIcon';
import { formatBytes, formatVersion } from '../../utils/formatters';

function getPlatform(titleId) {
  const normalizedTitleId = (titleId || '').trim().toUpperCase();
  if (normalizedTitleId.startsWith('PPSA')) return 'PS5';
  if (normalizedTitleId.startsWith('CUSA')) return 'PS4';
  return null;
}

export default function PackageGridView({ groupedTitles, searchQuery, onSearch, sortBy, onSort, onOpenTitle, selectedDrive, onBack, settings, installerStatus, loadingPackages, packages = [] }) {
  const setSearchQuery = onSearch;
  const setSortBy = onSort;
  const handleOpenTitle = onOpenTitle;
  const handleBackToDrives = onBack;
  const sDrive = selectedDrive || { label: 'Storage', path: '' };
  
  return (
    <div className="space-y-6">
      {/* Header & Back Button */}
      <div className="flex flex-col md:flex-row md:items-center justify-between space-y-4 md:space-y-0 md:space-x-4 border-b border-white/10 pb-6">
              <div className="flex items-center space-x-4">
                <button
                  type="button"
                  onClick={handleBackToDrives}
                  className="px-4 py-2.5 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 border border-white/10 text-sm font-semibold transition-all flex items-center space-x-2 text-zinc-200 cursor-pointer shrink-0"
                >
                  <span>&larr;</span>
                  <span>Back to Drives</span>
                </button>

                <div>
                  <h2 className="text-2xl font-black text-white flex items-center space-x-2">
                    <span>{sDrive.label}</span>
                    <span className="text-xs px-2.5 py-0.5 rounded-[2px] bg-blue-500/20 text-blue-400 font-mono">
                      {sDrive.path}
                    </span>
                  </h2>
                  <p className="text-xs text-zinc-400 mt-0.5">
                    {groupedTitles.length} {groupedTitles.length === 1 ? 'Title' : 'Titles'} ({packages.length} Packages) found
                  </p>
                </div>
              </div>

              {/* Search Bar & Sort Controls */}
              <div className="flex items-center space-x-3 shrink-0">
                <div className="flex items-center bg-[#161722] border border-white/10 rounded-[2px] ps5-focus-item px-3.5 py-2 w-64 focus-within:border-white/30">
                  <svg
                    className="w-4 h-4 text-zinc-500 mr-2.5 shrink-0"
                    viewBox="0 0 24 24"
                    fill="none"
                    stroke="currentColor"
                    strokeWidth="2"
                  >
                    <circle cx="11" cy="11" r="8" />
                    <line x1="21" y1="21" x2="16.65" y2="16.65" />
                  </svg>
                  <input
                    type="text"
                    placeholder="Search titles..."
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                    className="bg-transparent border-none outline-none text-sm text-white placeholder-zinc-500 w-full"
                  />
                </div>

                <div className="flex items-center space-x-2 bg-[#161722] border border-white/10 rounded-[2px] ps5-focus-item px-3 py-2 shrink-0">
                  <svg className="w-4 h-4 text-zinc-400 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                    <path d="M3 6h18M6 12h12M10 18h4" />
                  </svg>
                  <label htmlFor="sort-dropdown" className="text-xs text-zinc-400 shrink-0 font-medium">Sort:</label>
                  <select
                    id="sort-dropdown"
                    value={sortBy}
                    onChange={(e) => setSortBy(e.target.value)}
                    className="bg-transparent text-xs text-white focus:outline-none cursor-pointer pr-1"
                  >
                    <option value="date-desc" className="bg-[#161722] text-white">Date (Newest)</option>
                    <option value="date-asc" className="bg-[#161722] text-white">Date (Oldest)</option>
                    <option value="name-asc" className="bg-[#161722] text-white">Title (A-Z)</option>
                    <option value="name-desc" className="bg-[#161722] text-white">Title (Z-A)</option>
                    <option value="size-desc" className="bg-[#161722] text-white">Size (Largest)</option>
                    <option value="size-asc" className="bg-[#161722] text-white">Size (Smallest)</option>
                  </select>
                </div>
              </div>
            </div>

            {/* Grouped Titles Grid */}
            {loadingPackages ? (
              <div className="py-24 text-center">
                <div className="ps5-robust-spinner mx-auto" />
                <p className="text-sm text-zinc-400 mt-3">Loading packages from {selectedDrive.label}...</p>
              </div>
            ) : groupedTitles.length === 0 ? (
              <div className="py-16 text-center rounded-[2px] border border-white/10 bg-[#12131a]/40 p-8">
                <h3 className="text-lg font-bold text-white">No Matching Packages</h3>
                <p className="text-sm text-zinc-400 max-w-md mx-auto mt-2">
                  No packages matched your search query on {selectedDrive.label}.
                </p>
              </div>
            ) : (
              <div className="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 gap-5">
                {groupedTitles.map((group, index) => {
                  const isBaseInstalled = group.isBaseInstalled;
                  const hasBaseOnDrive = group.hasBaseOnDrive;
                  const hasNewBase = group.hasNewBase;
                  const isLatestUpdateInstalled = group.isLatestUpdateInstalled;
                  const hasNewUpdate = group.hasNewUpdate;
                  const areAllDlcsInstalled = group.areAllDlcsInstalled;
                  const hasNewDlc = group.hasNewDlc;
                  const isEverythingInstalled = group.isEverythingInstalled;
                  const platform = getPlatform(group.title_id);

                  return (
                    <div
                      key={group.id}
                      role="button"
                      tabIndex={0}
                      onClick={() => handleOpenTitle(group.id)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          e.preventDefault();
                          handleOpenTitle(group.id);
                        }
                      }}
                      className={`w-full group block text-left rounded-[2px] ps5-focus-item p-2.5 border transition-all cursor-pointer ${
                        (settings.fade_installed_packages && isEverythingInstalled)
                          ? 'card-darked-out'
                          : 'bg-[#141520] hover:bg-[#171824] border-white/10 hover:border-white/20'
                      }`}
                    >
                      {/* Square Image Box (with fallback for Safari <15) */}
                      <div className="aspect-square-box rounded-[2px] overflow-hidden bg-black/50 border border-white/10">
                        <div className="aspect-square-content overflow-hidden">
                          {/* Fallback Icon */}
                          <div className="absolute inset-0 flex items-center justify-center text-zinc-600 pointer-events-none">
                            <svg className="w-12 h-12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                              <rect x="2" y="3" width="20" height="14" rx="2" />
                              <line x1="8" y1="21" x2="16" y2="21" />
                              <line x1="12" y1="17" x2="12" y2="21" />
                            </svg>
                          </div>

                          {/* Package Image */}
                          {group.iconPath ? (
                            <BlurIcon
                              pkg={group.imagePkg}
                              alt={group.title_name}
                              priority={index < 10}
                              imgClassName="absolute inset-0 w-full h-full object-cover z-10 block"
                            />
                          ) : null}

                          {/* Installed badge on top-left if base is installed */}
                          {group.isBaseInstalled && (
                            <span className="absolute top-2 left-2 z-20 px-2 py-0.5 rounded-[2px] bg-emerald-600/90 text-[10px] font-bold text-white border border-emerald-400/30">
                              INSTALLED
                            </span>
                          )}

                          {/* Platform badge from the title ID */}
                          {platform && (
                            <span className={`absolute top-2 right-2 z-20 px-2 py-0.5 rounded-[2px] text-[10px] font-bold border pointer-events-none ${
                              platform === 'PS5'
                                ? 'bg-white text-black border-white'
                                : 'bg-zinc-900 text-zinc-200 border-zinc-600'
                            }`}>
                              {platform}
                            </span>
                          )}

                          {/* Leftover badge on top-left if base is missing but leftovers exist */}
                          {!group.isBaseInstalled && group.hasLeftover && (
                            <span
                              className="absolute top-2 left-2 z-20 px-2 py-0.5 rounded-[2px] bg-amber-600/95 text-[10px] font-bold text-white border border-amber-400/40 flex items-center space-x-1"
                              title={group.leftoverDesc || 'Leftover update or DLC exists on console without base package'}
                            >
                              <svg className="w-3 h-3 text-amber-200" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
                                <path strokeLinecap="round" strokeLinejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
                              </svg>
                              <span>LEFTOVER</span>
                            </span>
                          )}

                          {/* Aborted / Partially installed badge on top-left if install was aborted */}
                          {!group.isBaseInstalled && !group.hasLeftover && group.isPartiallyInstalled && (
                            <span
                              className="absolute top-2 left-2 z-20 px-2 py-0.5 rounded-[2px] bg-rose-600/95 text-[10px] font-bold text-white border border-rose-400/40 flex items-center space-x-1"
                              title={group.partialDesc || 'Partially installed or aborted installation detected on console'}
                            >
                              <svg className="w-3 h-3 text-rose-200" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
                                <path strokeLinecap="round" strokeLinejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
                              </svg>
                              <span>ABORTED</span>
                            </span>
                          )}

                          {/* Source tag on bottom-left of square image (only in All Sources mode) */}
                          {selectedDrive.id === '__all__' && group.sourceName && (
                            <div className="absolute bottom-2 left-2 z-20 pointer-events-none max-w-[45%]">
                              <span className={`text-[10px] font-bold px-1.5 py-0.5 rounded-[2px] border truncate block ${
                                group.sourceType === 'smb'
                                  ? 'bg-cyan-950/85 text-cyan-300 border-cyan-500/50'
                                  : group.sourceType === 'disc'
                                  ? 'bg-purple-950/85 text-purple-300 border-purple-500/50'
                                  : 'bg-zinc-800 text-zinc-300 border-white/20'
                              }`}>
                                {group.sourceName}
                              </span>
                            </div>
                          )}

                          {/* Tags on bottom-right of square image */}
                          <div className="absolute bottom-2 right-2 z-20 flex flex-col items-end space-y-1 pointer-events-none">
                            {group.base && (
                              <span className={`text-[10px] font-bold px-1.5 py-0.5 rounded-[2px] border ${
                                hasNewBase
                                  ? 'bg-blue-600 text-white border-blue-400/60'
                                  : 'bg-blue-950/70 text-blue-300 border-blue-800/40'
                              }`}>
                                base
                              </span>
                            )}
                            {group.updates.length > 0 && (
                              <span className={`text-[10px] font-mono px-1.5 py-0.5 rounded-[2px] border ${
                                hasNewUpdate
                                  ? 'bg-purple-600 text-white border-purple-300 font-bold'
                                  : 'bg-purple-950/70 text-purple-300/80 border-purple-800/40 font-medium'
                              }`}>
                                {hasNewUpdate ? 'NEW: ' : ''}update{group.latestUpdateVersion ? ` ${group.latestUpdateVersion}` : ''}
                              </span>
                            )}
                            {group.dlcCount > 0 && (
                              <span className={`text-[10px] px-1.5 py-0.5 rounded-[2px] border ${
                                hasNewDlc
                                  ? 'bg-emerald-600 text-white border-emerald-300 font-bold'
                                  : 'bg-emerald-950/70 text-emerald-300/80 border-emerald-800/40 font-medium'
                              }`}>
                                {hasNewDlc ? 'NEW: ' : ''}{group.dlcCount} {group.dlcCount === 1 ? 'DLC' : 'DLCs'}
                              </span>
                            )}
                          </div>
                        </div>
                      </div>

                      {/* Title Below Image */}
                      <h3
                        className="text-sm font-bold text-white truncate mt-2 w-full transition-colors"
                        title={group.title_name}
                      >
                        {group.title_name}
                      </h3>

                      {/* Subtitle / ID & Size */}
                      <div className="text-xs text-zinc-400 font-mono flex items-center justify-between mt-0.5 w-full">
                        <span>{group.title_id || 'PKG'}</span>
                        <span className="text-zinc-500">
                          {group.hasMultipart ? `${formatBytes(group.totalDriveSize)} / ${formatBytes(group.totalFullSize)}` : formatBytes(group.totalSize)}
                        </span>
                      </div>
                    </div>
                  );
                })}
              </div>
            )}
          </div>
  );
}
