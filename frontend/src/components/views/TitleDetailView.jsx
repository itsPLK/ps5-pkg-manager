import React from 'react';
import BlurIcon from '../../BlurIcon';
import { formatBytes, formatVersion } from '../../utils/formatters';
import { getInstallStorageOptions } from '../../utils/installStorage';

export default function TitleDetailView({ title: selectedTitle, onBack, onInstall, onInstallBaseAndUpdate, onOpenLeftoverCleanup, installerStatus, storage, settings, drives, selectedDrive }) {
  const handleBackToPackages = onBack;
  const handleInstall = onInstall;
  const handleInstallBaseAndUpdate = onInstallBaseAndUpdate;
  const handleOpenLeftoverCleanupForTitle = onOpenLeftoverCleanup;
  const sDrive = selectedDrive || { id: '__all__', label: 'All Sources' };
  const maxAvailableFor = (titleId) => getInstallStorageOptions(storage, titleId)
    .reduce((max, option) => Math.max(max, option.free), 0);
  
  return (
          <div className="max-w-6xl mx-auto px-4 sm:px-8 space-y-6 pb-12">
            {/* Back to Packages Button */}
            <div className="flex items-center space-x-4 border-b border-white/10 pb-4">
              <button
                type="button"
                onClick={handleBackToPackages}
                className="px-5 py-2.5 rounded-[2px] ps5-focus-item bg-white/5 hover:bg-white/10 border border-white/10 text-base font-semibold transition-all flex items-center space-x-2 text-zinc-200 shrink-0 cursor-pointer"
              >
                <span>&larr;</span>
                <span>Back to Packages</span>
              </button>

              <div className="text-sm text-zinc-400 flex items-center space-x-2 min-w-0">
                <span className="shrink-0">
                  {sDrive.id === '__all__'
                    ? (selectedTitle.sourceName ? `All Sources (${selectedTitle.sourceName})` : 'All Sources')
                    : sDrive.label}
                </span>
                <span className="shrink-0 text-zinc-600">&rsaquo;</span>
                <span className="text-white font-semibold truncate">{selectedTitle.title_name}</span>
              </div>
            </div>

            {/* Title Hero Card */}
            <div className="rounded-[2px] bg-[#141520] border border-white/10 p-7 flex flex-col space-y-6">
              <div className="flex flex-col md:flex-row items-start md:items-center justify-between space-y-6 md:space-y-0 md:space-x-8 w-full">
                <div className="flex flex-col sm:flex-row items-start sm:items-center space-y-4 sm:space-y-0 sm:space-x-7 min-w-0 flex-1">
                  {/* Large Base / Representative Image */}
                  <div className="w-40 h-40 sm:w-48 sm:h-48 rounded-[2px] overflow-hidden bg-black/50 border border-white/10 shrink-0 relative">
                    <div className="absolute inset-0 flex items-center justify-center text-zinc-600 pointer-events-none">
                      <svg className="w-14 h-14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                        <rect x="2" y="3" width="20" height="14" rx="2" />
                        <line x1="8" y1="21" x2="16" y2="21" />
                        <line x1="12" y1="17" x2="12" y2="21" />
                      </svg>
                    </div>
                    {selectedTitle.iconPath ? (
                      <BlurIcon
                        pkg={selectedTitle.imagePkg}
                        alt={selectedTitle.title_name}
                        priority={true}
                        imgClassName="absolute inset-0 w-full h-full object-cover z-10 block"
                      />
                    ) : null}
                  </div>

                  {/* Metadata */}
                  <div className="min-w-0 flex-1">
                    <h2 className="text-3xl sm:text-4xl font-black text-white leading-tight">
                      {selectedTitle.title_name}
                    </h2>

                    <div className="flex items-center space-x-2.5 mt-3 flex-wrap text-xs sm:text-sm">
                      {selectedTitle.title_id ? (
                        <span className="font-mono px-3 py-1 my-0.5 rounded-[2px] bg-white/5 text-zinc-200 border border-white/10 shrink-0 font-medium">
                          {selectedTitle.title_id}
                        </span>
                      ) : null}

                      {selectedDrive.id === '__all__' && selectedTitle.sourceName && (
                        <span className={`px-3 py-1 my-0.5 rounded-[2px] font-bold border shrink-0 text-xs sm:text-sm ${
                          selectedTitle.sourceType === 'smb'
                            ? 'bg-cyan-500/20 text-cyan-300 border-cyan-500/30'
                            : selectedTitle.sourceType === 'disc'
                            ? 'bg-purple-500/20 text-purple-300 border-purple-500/30'
                            : 'bg-blue-500/20 text-blue-300 border-blue-500/30'
                        }`}>
                          {selectedTitle.sourceName}
                        </span>
                      )}

                      {selectedTitle.hasMultipart ? (
                        <>
                          <span className="font-mono px-3 py-1 my-0.5 rounded-[2px] bg-white/5 text-zinc-200 border border-white/10 shrink-0 font-medium">
                            Size: {formatBytes(selectedTitle.totalDriveSize)} (Drive) / {formatBytes(selectedTitle.totalFullSize)} Full
                          </span>
                          {selectedTitle.isBaseMultipart && (
                            <span className="px-3 py-1 my-0.5 rounded-[2px] bg-blue-500/20 text-blue-300 border border-blue-500/30 font-bold shrink-0">
                              Base {selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of {selectedTitle.totalParts}
                            </span>
                          )}
                        </>
                      ) : (
                        <span className="font-mono px-3 py-1 my-0.5 rounded-[2px] bg-white/5 text-zinc-200 border border-white/10 shrink-0 font-medium">
                          Total: {formatBytes(selectedTitle.totalSize)}
                        </span>
                      )}

                      {selectedTitle.isBaseInstalled ? (
                        <span className="px-3 py-1 my-0.5 rounded-[2px] bg-emerald-500/20 text-emerald-300 border border-emerald-500/30 font-bold shrink-0">
                          Base Installed{selectedTitle.installedVersion ? ` (${selectedTitle.installedVersion})` : ''}
                        </span>
                      ) : selectedTitle.hasLeftover ? (
                        <span className="px-3 py-1 my-0.5 rounded-[2px] bg-amber-500/20 text-amber-300 border border-amber-500/30 font-bold shrink-0 flex items-center space-x-1.5">
                          <svg className="w-3.5 h-3.5 text-amber-300" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
                            <path strokeLinecap="round" strokeLinejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
                          </svg>
                          <span>Leftovers Found</span>
                        </span>
                      ) : null}
                    </div>

                    {(!selectedTitle.isBaseInstalled && !selectedTitle.base) ? (
                      <p className="text-xs text-amber-300 mt-3 font-medium">
                        Base package is required to install updates and DLC.
                      </p>
                    ) : (selectedTitle.base && selectedTitle.isMultipart && (!selectedTitle.base.is_installed || selectedTitle.base.can_install !== false)) ? (
                      <p className="text-xs text-zinc-400 mt-3">
                        Subsequent {selectedTitle.sourceType === 'disc' ? 'discs' : 'parts'} will be requested during installation.
                      </p>
                    ) : null}
                  </div>
                </div>

                {/* Base Package Action Area */}
                <div className="shrink-0 w-full sm:w-auto">
                  {selectedTitle.base ? (
                    (selectedTitle.base.is_installed && selectedTitle.base.can_install === false) ? (
                      <div className="px-6 py-3 rounded-[2px] bg-emerald-500/15 border border-emerald-500/30 text-emerald-300 text-base font-bold flex items-center justify-center space-x-2 whitespace-nowrap">
                        <svg className="w-5 h-5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5">
                          <polyline points="20 6 9 17 4 12" />
                        </svg>
                        <span>Base Installed</span>
                      </div>
                    ) : (
                      <div className="flex flex-col items-stretch space-y-2.5">
                        {selectedTitle.updates.length > 0 && (
                          <button
                            type="button"
                            onClick={() => handleInstallBaseAndUpdate(selectedTitle.base, selectedTitle.updates[0])}
                            disabled={installerStatus.is_installing || selectedTitle.hasLeftover || selectedTitle.base.can_install === false}
                            title={selectedTitle.hasLeftover ? 'Leftovers detected on console. Clean up leftovers before installing.' : (selectedTitle.base.install_disabled_reason || '')}
                            className={`w-full px-6 py-3.5 rounded-[2px] ps5-focus-item font-bold text-base transition-all flex items-center justify-center space-x-2.5 whitespace-nowrap ${
                              (selectedTitle.hasLeftover || selectedTitle.base.can_install === false)
                                ? 'bg-zinc-800/80 text-zinc-500 border border-white/5 cursor-not-allowed'
                                : 'bg-gradient-to-r from-blue-600 to-purple-600 hover:from-blue-500 hover:to-purple-500 text-white disabled:opacity-50 cursor-pointer'
                            }`}
                          >
                            <svg className="w-5 h-5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                              <polyline points="7 10 12 15 17 10" />
                              <line x1="12" y1="15" x2="12" y2="3" />
                            </svg>
                            <span>
                              {selectedTitle.base.is_installed ? 'Upgrade ' : selectedTitle.isPartiallyInstalled ? 'Reinstall ' : 'Install '}
                              {selectedTitle.isMultipart ? `${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${selectedTitle.totalParts} + Update` : 'Base + Update'}
                              {' '}(
                              {selectedTitle.isMultipart
                                ? `${formatBytes(selectedTitle.firstPartSize + selectedTitle.updates[0].file_size)} / ${formatBytes(selectedTitle.baseFullSize + selectedTitle.updates[0].file_size)}`
                                : formatBytes(selectedTitle.base.file_size + selectedTitle.updates[0].file_size)}
                              )
                            </span>
                          </button>
                        )}
                        <button
                          type="button"
                          onClick={() => handleInstall(selectedTitle.base)}
                          disabled={installerStatus.is_installing || selectedTitle.hasLeftover || selectedTitle.base.can_install === false}
                          title={selectedTitle.hasLeftover ? 'Leftovers detected on console. Clean up leftovers before installing.' : (selectedTitle.base.install_disabled_reason || '')}
                          className={`w-full px-5 py-3.5 rounded-[2px] ps5-focus-item font-bold text-base transition-all flex items-center justify-center space-x-2.5 whitespace-nowrap ${
                            (selectedTitle.hasLeftover || selectedTitle.base.can_install === false)
                              ? 'bg-zinc-800/80 text-zinc-500 border border-white/5 cursor-not-allowed'
                              : 'bg-blue-600/80 hover:bg-blue-600 text-white disabled:opacity-50 cursor-pointer border border-white/10'
                          }`}
                        >
                          <svg className="w-5 h-5 shrink-0" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                            <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                            <polyline points="7 10 12 15 17 10" />
                            <line x1="12" y1="15" x2="12" y2="3" />
                          </svg>
                          <span>
                            {selectedTitle.base.is_installed ? 'Upgrade ' : selectedTitle.isPartiallyInstalled ? 'Reinstall ' : 'Install '}
                            {selectedTitle.isMultipart
                              ? `${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${selectedTitle.totalParts}${selectedTitle.updates.length > 0 ? ' Only' : ''}`
                              : (selectedTitle.updates.length > 0
                                  ? `Base ${formatVersion(selectedTitle.base.app_version) || 'v1.00'} Only`
                                  : `Base ${formatVersion(selectedTitle.base.app_version) || 'v1.00'}`)}
                            {' '}(
                            {selectedTitle.isMultipart
                              ? `${formatBytes(selectedTitle.firstPartSize)} / ${formatBytes(selectedTitle.baseFullSize)}`
                              : formatBytes(selectedTitle.base.file_size)}
                            )
                          </span>
                        </button>
                      </div>
                    )
                  ) : (
                    <div className="px-5 py-3 rounded-[2px] bg-white/5 border border-white/10 text-sm font-medium text-zinc-400 text-center whitespace-nowrap">
                      No Base PKG on Drive
                    </div>
                  )}
                </div>
              </div>

              {/* Leftover Alert Banner in Detail View */}
              {!selectedTitle.isBaseInstalled && selectedTitle.hasLeftover && (
                <div className="w-full pt-4 border-t border-white/10">
                  <div className="p-4 sm:p-5 rounded-[2px] bg-amber-500/15 border border-amber-500/30 text-amber-200 text-xs sm:text-sm flex flex-col sm:flex-row sm:items-center justify-between gap-4">
                    <div className="flex items-start space-x-3.5 min-w-0 flex-1">
                      <svg className="w-5 h-5 text-amber-400 shrink-0 mt-0.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <path strokeLinecap="round" strokeLinejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
                      </svg>
                      <div className="space-y-1">
                        <div className="font-bold text-amber-300 text-sm sm:text-base">Leftovers Detected on Console</div>
                        <div className="text-zinc-300 leading-relaxed">
                          {selectedTitle.leftoverDesc ? `${selectedTitle.leftoverDesc}. ` : 'Files from a previous installation were found without a base package. '}
                          Clean up leftovers before installing.
                        </div>
                      </div>
                    </div>
                    <button
                      type="button"
                      onClick={() => handleOpenLeftoverCleanupForTitle(selectedTitle.title_id, selectedTitle.title_name)}
                      className="shrink-0 px-5 py-2.5 rounded-[2px] ps5-focus-item bg-amber-500 hover:bg-amber-400 text-black font-bold text-xs sm:text-sm transition-colors flex items-center justify-center space-x-2 cursor-pointer whitespace-nowrap"
                    >
                      <svg className="w-4 h-4" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <polyline points="3 6 5 6 21 6" />
                        <path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                      </svg>
                      <span>Clean Up Leftovers</span>
                    </button>
                  </div>
                </div>
              )}

              {/* Partially Installed / Aborted Alert Banner in Detail View */}
              {!selectedTitle.isBaseInstalled && !selectedTitle.hasLeftover && selectedTitle.isPartiallyInstalled && (
                <div className="w-full pt-4 border-t border-white/10">
                  <div className="p-4 sm:p-5 rounded-[2px] bg-rose-500/15 border border-rose-500/30 text-rose-200 text-xs sm:text-sm flex flex-col sm:flex-row sm:items-center justify-between gap-4">
                    <div className="flex items-start space-x-3.5 min-w-0 flex-1">
                      <svg className="w-5 h-5 text-rose-400 shrink-0 mt-0.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
                        <path strokeLinecap="round" strokeLinejoin="round" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z" />
                      </svg>
                      <div className="space-y-1">
                        <div className="font-bold text-rose-300 text-sm sm:text-base">Incomplete Installation Detected</div>
                        <div className="text-zinc-300 leading-relaxed">
                          An aborted installation was detected on the console. Delete the broken icon from your PS5 home screen (Options &rarr; Delete) or reinstall the base package.
                        </div>
                      </div>
                    </div>
                  </div>
                </div>
              )}
            </div>

            {/* Updates Section */}
            {selectedTitle.updates.length > 0 && (
              <div className="space-y-3">
                <h3 className="text-xl font-bold text-white flex items-center space-x-2.5">
                  <span>Updates</span>
                  <span className="text-xs px-2.5 py-1 rounded-[2px] bg-purple-500/20 text-purple-300 border border-purple-500/30 font-bold">
                    {selectedTitle.updates.length} Available
                  </span>
                </h3>

                <div className="space-y-2.5">
                  {selectedTitle.updates.map((pkg) => {
                    const isUpdMultipart = !!pkg.is_multipart && (Number(pkg.total_parts) > 1);
                    const updTotalParts = Number(pkg.total_parts) || 1;
                    const updFullSize = Number(pkg.total_pkg_size || pkg.file_size) || 0;
                    const requiredSpace = updFullSize;
                    const maxAvailable = maxAvailableFor(pkg.title_id || selectedTitle.title_id);
                    const notEnoughSpace = !!storage && maxAvailable < requiredSpace;
                    const canInstall = pkg.can_install !== false;
                    const isInstallDisabled = !canInstall || notEnoughSpace || installerStatus.is_installing;

                    let disabledLabel = 'Unavailable';
                    if (selectedTitle.hasLeftover || (pkg.install_disabled_reason && pkg.install_disabled_reason.includes('Leftovers detected'))) {
                      disabledLabel = 'Leftovers Found';
                    } else if (pkg.install_disabled_reason && pkg.install_disabled_reason.includes('aborted')) {
                      disabledLabel = 'Base Aborted';
                    } else if (pkg.install_disabled_reason && (pkg.install_disabled_reason.includes('Base package is not installed') || pkg.install_disabled_reason.includes('Base game is not installed'))) {
                      disabledLabel = 'Base Required';
                    } else if (pkg.install_disabled_reason === 'Installed version is same or newer') {
                      disabledLabel = 'Up to Date';
                    } else if (!pkg.is_installed) {
                      disabledLabel = 'Base Required';
                    } else if (pkg.can_install === false) {
                      disabledLabel = 'Up to Date';
                    }

                    return (
                      <div
                        key={pkg.path}
                        className="rounded-[2px] p-5 bg-[#141520] border border-white/10 flex items-center justify-between space-x-5"
                      >
                        <div className="flex items-center space-x-5 min-w-0 flex-1">
                          <div className="w-14 h-14 rounded-[2px] overflow-hidden bg-black/50 border border-purple-500/30 shrink-0 flex items-center justify-center relative">
                            {pkg.has_icon ? (
                              <BlurIcon
                                pkg={pkg}
                                alt={pkg.title_name || 'Update'}
                                priority={true}
                                imgClassName="absolute inset-0 w-full h-full object-cover z-10 block"
                              />
                            ) : null}
                            <div className="w-full h-full bg-purple-950/40 flex items-center justify-center text-purple-300 font-black text-xs tracking-wider">
                              UPDATE
                            </div>
                          </div>

                          <div className="min-w-0 flex-1">
                            <div className="flex items-center space-x-2.5 flex-wrap gap-y-1">
                              <span className="text-base font-bold text-white">
                                {pkg.app_version ? `Update ${formatVersion(pkg.app_version)}` : 'Package Update'}
                              </span>
                              {isUpdMultipart ? (
                                <>
                                  <span className="text-xs sm:text-sm font-mono text-zinc-300 shrink-0 font-medium">
                                    ({formatBytes(pkg.file_size)} / {formatBytes(updFullSize)})
                                  </span>
                                  <span className="text-[10px] font-bold px-2 py-0.5 rounded-[2px] bg-purple-500/20 text-purple-300 border border-purple-500/30 shrink-0">
                                    {selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of {updTotalParts}
                                  </span>
                                </>
                              ) : (
                                <span className="text-xs sm:text-sm font-mono text-zinc-400 shrink-0">
                                  ({formatBytes(pkg.file_size)})
                                </span>
                              )}
                            </div>

                            {isUpdMultipart && (
                              <p className="text-xs text-purple-300/80 mt-1">
                                Subsequent {selectedTitle.sourceType === 'disc' ? 'discs' : 'parts'} will be requested during installation.
                              </p>
                            )}

                            {!canInstall && pkg.install_disabled_reason ? (
                              <p className="text-xs text-amber-400 mt-1">
                                • {pkg.install_disabled_reason}
                              </p>
                            ) : null}
                          </div>
                        </div>

                        <div className="shrink-0">
                          <button
                            type="button"
                            onClick={() => !isInstallDisabled && handleInstall(pkg)}
                            disabled={isInstallDisabled}
                            className={`px-5 py-2.5 rounded-[2px] ps5-focus-item text-sm font-bold transition-all whitespace-nowrap ${
                              isInstallDisabled
                                ? 'bg-zinc-800 text-zinc-500 border border-white/5 cursor-not-allowed'
                                : 'bg-purple-600 hover:bg-purple-500 text-white cursor-pointer'
                            }`}
                          >
                            {notEnoughSpace
                              ? 'No Space'
                              : !canInstall
                              ? disabledLabel
                              : isUpdMultipart
                              ? `Install ${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${updTotalParts}`
                              : 'Install Update'}
                          </button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            )}

            {/* DLCs Section */}
            {selectedTitle.dlcs.length > 0 && (
              <div className="space-y-3">
                <h3 className="text-xl font-bold text-white flex items-center space-x-2.5">
                  <span>Downloadable Content (DLC)</span>
                  <span className="text-xs px-2.5 py-1 rounded-[2px] bg-emerald-500/20 text-emerald-300 border border-emerald-500/30 font-bold">
                    {selectedTitle.dlcs.length} Available
                  </span>
                </h3>

                <div className="grid grid-cols-2 sm:grid-cols-3 md:grid-cols-4 lg:grid-cols-5 gap-5">
                  {selectedTitle.dlcs.map((pkg, index) => {
                    const isDlcMultipart = !!pkg.is_multipart && (Number(pkg.total_parts) > 1);
                    const dlcTotalParts = Number(pkg.total_parts) || 1;
                    const dlcFullSize = Number(pkg.total_pkg_size || pkg.file_size) || 0;
                    const requiredSpace = dlcFullSize;
                    const maxAvailable = maxAvailableFor(pkg.title_id || selectedTitle.title_id);
                    const notEnoughSpace = !!storage && maxAvailable < requiredSpace;
                    const canInstall = pkg.can_install !== false;
                    const isInstallDisabled = !canInstall || notEnoughSpace || installerStatus.is_installing;

                    let disabledLabel = 'Unavailable';
                    if (selectedTitle.hasLeftover || (pkg.install_disabled_reason && pkg.install_disabled_reason.includes('Leftovers detected'))) {
                      disabledLabel = 'Leftovers Found';
                    } else if (pkg.install_disabled_reason && pkg.install_disabled_reason.includes('aborted')) {
                      disabledLabel = 'Base Aborted';
                    } else if (
                      pkg.install_disabled_reason === 'Base package is not installed' ||
                      pkg.install_disabled_reason === 'Base game is not installed' ||
                      (pkg.install_disabled_reason && (pkg.install_disabled_reason.includes('Base package is not installed') || pkg.install_disabled_reason.includes('Base game is not installed'))) ||
                      !pkg.is_installed
                    ) {
                      disabledLabel = 'Base Required';
                    } else if (pkg.is_dlc_installed || pkg.install_disabled_reason === 'DLC is already installed') {
                      disabledLabel = 'Installed';
                    }

                    return (
                      <div
                        key={pkg.path}
                        onClick={(e) => {
                          if (!isInstallDisabled && e.target.tagName !== 'BUTTON') {
                            handleInstall(pkg);
                          }
                        }}
                        className={`w-full group flex flex-col justify-between text-left rounded-[2px] p-2.5 border transition-all ${
                          isInstallDisabled ? '' : 'cursor-pointer ps5-focus-item'
                        } ${
                          (settings.fade_installed_packages && pkg.is_dlc_installed)
                            ? 'card-darked-out'
                            : 'bg-[#141520] hover:bg-[#171824] border-white/10 hover:border-white/20'
                        }`}
                      >
                        <div>
                          {/* Square Image Box (with fallback for Safari <15) */}
                          <div className="aspect-square-box rounded-[2px] overflow-hidden bg-black/50 border border-white/10">
                            <div className="aspect-square-content overflow-hidden">
                              {/* Fallback Icon */}
                              <div className="absolute inset-0 flex items-center justify-center text-zinc-600 pointer-events-none">
                                <svg className="w-12 h-12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5">
                                  <polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2" />
                                </svg>
                              </div>

                              {/* DLC Image */}
                              {pkg.has_icon ? (
                                <BlurIcon
                                  pkg={pkg}
                                  alt={pkg.title_name || 'DLC'}
                                  priority={index < 8}
                                  imgClassName="absolute inset-0 w-full h-full object-cover z-10 block"
                                />
                              ) : null}

                              {/* Installed badge on top-left if DLC is installed */}
                              {pkg.is_dlc_installed && (
                                <span className="absolute top-2 left-2 z-20 px-2 py-0.5 rounded-[2px] bg-emerald-600/90 text-[10px] font-bold text-white border border-emerald-400/30">
                                  INSTALLED
                                </span>
                              )}

                              {/* Tag on bottom-right of square image */}
                              <div className="absolute bottom-2 right-2 z-20 flex flex-col items-end space-y-1 pointer-events-none">
                                <span className="text-[10px] font-bold px-1.5 py-0.5 rounded-[2px] border bg-emerald-950/70 text-emerald-300/80 border-emerald-800/40">
                                  {isDlcMultipart ? `${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${dlcTotalParts}` : 'DLC'}
                                </span>
                              </div>
                            </div>
                          </div>

                          {/* Title Below Image */}
                          <h4
                            className="text-sm font-bold text-white truncate mt-2 w-full transition-colors"
                            title={pkg.title_name || 'DLC'}
                          >
                            {pkg.title_name || 'DLC'}
                          </h4>

                          {/* Size */}
                          <div className="text-xs text-zinc-400 font-mono flex items-center justify-between mt-0.5 w-full">
                            <span>DLC</span>
                            <span className="text-zinc-500">
                              {isDlcMultipart ? `${formatBytes(pkg.file_size)} / ${formatBytes(dlcFullSize)}` : formatBytes(pkg.file_size)}
                            </span>
                          </div>

                          {/* Warning (if any) */}
                          {!canInstall && pkg.install_disabled_reason ? (
                            <p className="text-[11px] text-amber-400 mt-1 truncate" title={pkg.install_disabled_reason}>
                              • {pkg.install_disabled_reason}
                            </p>
                          ) : null}
                        </div>

                        {/* Action Button */}
                        <div className="mt-3">
                          <button
                            type="button"
                            onClick={(e) => {
                              e.stopPropagation();
                              if (!isInstallDisabled) handleInstall(pkg);
                            }}
                            disabled={isInstallDisabled}
                            className={`w-full py-2 px-2 rounded-[2px] ps5-focus-item text-xs sm:text-sm font-bold transition-all text-center truncate ${
                              isInstallDisabled
                                ? 'bg-zinc-800 text-zinc-500 border border-white/5 cursor-not-allowed'
                                : 'bg-emerald-600 hover:bg-emerald-500 text-white cursor-pointer'
                            }`}
                          >
                            {notEnoughSpace ? 'No Space' : !canInstall ? disabledLabel : isDlcMultipart ? `Install ${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${dlcTotalParts}` : 'Install DLC'}
                          </button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            )}

            {/* Other Packages (if any) */}
            {selectedTitle.others.length > 0 && (
              <div className="space-y-3">
                <h3 className="text-xl font-bold text-white flex items-center space-x-2.5">
                  <span>Other Packages</span>
                  <span className="text-xs px-2.5 py-1 rounded-[2px] bg-white/10 text-zinc-300 border border-white/20 font-bold">
                    {selectedTitle.others.length}
                  </span>
                </h3>

                <div className="space-y-2.5">
                  {selectedTitle.others.map((pkg, index) => {
                    const isOtherMultipart = !!pkg.is_multipart && (Number(pkg.total_parts) > 1);
                    const otherTotalParts = Number(pkg.total_parts) || 1;
                    const otherFullSize = Number(pkg.total_pkg_size || pkg.file_size) || 0;
                    const requiredSpace = otherFullSize;
                    const maxAvailable = maxAvailableFor(pkg.title_id || selectedTitle.title_id);
                    const notEnoughSpace = !!storage && maxAvailable < requiredSpace;
                    const canInstall = pkg.can_install !== false;
                    const isInstallDisabled = !canInstall || notEnoughSpace || installerStatus.is_installing;

                    return (
                      <div
                        key={pkg.path}
                        className="rounded-[2px] p-5 bg-[#141520] border border-white/10 flex items-center justify-between space-x-5"
                      >
                        <div className="flex items-center space-x-5 min-w-0 flex-1">
                          <div className="w-14 h-14 rounded-[2px] overflow-hidden bg-black/50 border border-white/10 shrink-0 flex items-center justify-center relative">
                            {pkg.has_icon ? (
                              <BlurIcon
                                pkg={pkg}
                                alt={pkg.title_name || pkg.filename}
                                priority={index < 8}
                                imgClassName="absolute inset-0 w-full h-full object-cover z-10 block"
                              />
                            ) : null}
                            <div className="w-full h-full bg-white/5 flex items-center justify-center text-zinc-400 font-black text-xs tracking-wider">
                              PKG
                            </div>
                          </div>

                          <div className="min-w-0 flex-1">
                            <div className="flex items-center space-x-2.5 flex-wrap gap-y-1">
                              <span className="text-base font-bold text-white truncate" title={pkg.title_name || pkg.filename}>
                                {pkg.title_name || pkg.filename}
                              </span>
                              {isOtherMultipart && (
                                <span className="text-[10px] font-bold px-2 py-0.5 rounded-[2px] bg-white/10 text-zinc-300 border border-white/20 shrink-0">
                                  {selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of {otherTotalParts}
                                </span>
                              )}
                            </div>
                            <p className="text-xs sm:text-sm font-mono text-zinc-500 truncate mt-0.5">
                              {pkg.content_id || pkg.filename} • {isOtherMultipart ? `${formatBytes(pkg.file_size)} / ${formatBytes(otherFullSize)}` : formatBytes(pkg.file_size)}
                            </p>
                            {isOtherMultipart && (
                              <p className="text-xs text-zinc-400 mt-1">
                                Subsequent {selectedTitle.sourceType === 'disc' ? 'discs' : 'parts'} will be requested during installation.
                              </p>
                            )}
                          </div>
                        </div>

                        <div className="shrink-0">
                          <button
                            type="button"
                            onClick={() => !isInstallDisabled && handleInstall(pkg)}
                            disabled={isInstallDisabled}
                            className={`px-5 py-2.5 rounded-[2px] ps5-focus-item text-sm font-bold transition-all whitespace-nowrap ${
                              isInstallDisabled
                                ? 'bg-zinc-800 text-zinc-500 border border-white/5 cursor-not-allowed'
                                : 'bg-blue-600 hover:bg-blue-500 text-white cursor-pointer'
                            }`}
                          >
                            {notEnoughSpace ? 'No Space' : isOtherMultipart ? `Install ${selectedTitle.sourceType === 'disc' ? 'Disc' : 'Part'} 1 of ${otherTotalParts}` : 'Install'}
                          </button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            )}
          </div>
  );
}
