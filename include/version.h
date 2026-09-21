#ifndef VERSION_H
#define VERSION_H

#define PKGMGR_VERSION "1.2.0"

#ifndef PKGMGR_BUILD_COMMIT
#define PKGMGR_BUILD_COMMIT "unknown"
#endif

#ifndef PKGMGR_BUILD_DATE
#define PKGMGR_BUILD_DATE __DATE__ " " __TIME__
#endif

#endif /* VERSION_H */
