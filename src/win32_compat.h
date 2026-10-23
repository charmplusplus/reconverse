// Windows replacements for the few POSIX interfaces reconverse uses. Private to
// reconverse's sources; include it only under _WIN32. converse.h deliberately
// does not include <windows.h>, so code built on reconverse is not exposed to
// its macros.
#ifndef RECONVERSE_WIN32_COMPAT_H
#define RECONVERSE_WIN32_COMPAT_H

#ifndef _WIN32
#error "win32_compat.h is for Windows builds only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h> // GetProcessMemoryInfo

#include <direct.h>  // _mkdir
#include <process.h> // _beginthreadex
#include <string.h>

// strtok_s has strtok_r's signature and semantics.
#define strtok_r strtok_s

#endif // RECONVERSE_WIN32_COMPAT_H
