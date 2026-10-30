// Memory usage of the process: the CmiMemoryUsage family declared in
// converse.h, classic Converse's API. Every PE of a process sees the same
// figures, and the high- and low-water marks are shared by the process.
#include "converse_internal.h"

#include <atomic>
#include <cstdio>

#ifdef _WIN32
#include "win32_compat.h"
#else
#include <sys/resource.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif

// Where the platform can say so, usage is the bytes malloc has handed out and
// not had back. mallinfo2 is glibc 2.33 and later; mallinfo before it has int
// fields that wrap at 2 GiB. With jemalloc linked in, the system allocator's
// own heap is nearly empty, so neither glibc nor macOS is asked.
#if defined(__GLIBC__) && !defined(RECONVERSE_USE_JEMALLOC) &&                 \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#define CMI_USE_MALLINFO2 1
#include <malloc.h>
#endif

// The first probe that answers on this platform: bytes, and its name in
// *name; 0 and "none" if none does.
static CmiUInt8 probeMemoryUsage(const char **name) {
#if defined(_WIN32)
  // Committed private bytes (the pagefile-backed total, resident or not), the
  // figure classic Converse reported on Windows. kernel32 exports this as
  // K32GetProcessMemoryInfo, so no psapi.lib is needed.
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    *name = "Windows";
    return (CmiUInt8)pmc.PagefileUsage;
  }
#elif defined(__APPLE__)
#ifndef RECONVERSE_USE_JEMALLOC
  // Bytes in use across all malloc zones; classic Converse's mstats asked the
  // default zone only. The resident size below is no substitute where this
  // answers: freed large blocks stay resident in malloc's cache, so it does
  // not fall when they are freed.
  malloc_statistics_t stats;
  malloc_zone_statistics(NULL, &stats);
  if (stats.size_in_use) {
    *name = "malloc_zone_statistics";
    return (CmiUInt8)stats.size_in_use;
  }
#endif
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                &count) == KERN_SUCCESS) {
    *name = "task_info";
    return (CmiUInt8)info.resident_size;
  }
#else
#if CMI_USE_MALLINFO2
  // Bytes malloc has handed out and not had back: in its arenas (uordblks)
  // plus the blocks it mapped on their own (hblkhd). Classic Converse took the
  // larger of the two rather than their sum, which undercounts.
  const struct mallinfo2 mi = mallinfo2();
  const CmiUInt8 used = (CmiUInt8)mi.uordblks + (CmiUInt8)mi.hblkhd;
  if (used) {
    *name = "mallinfo2";
    return used;
  }
#endif
  // The resident set: the second field of statm, in pages.
  if (FILE *f = fopen("/proc/self/statm", "r")) {
    unsigned long long size = 0, resident = 0;
    const int n = fscanf(f, "%llu %llu", &size, &resident);
    fclose(f);
    if (n == 2 && resident) {
      *name = "/proc/self/statm";
      return (CmiUInt8)resident * (CmiUInt8)CmiGetPageSize();
    }
  }
#endif
  *name = "none";
  return 0;
}

// Highest and lowest CmiMemoryUsage seen since the last reset; 0 until the
// first sample.
static std::atomic<CmiUInt8> maxSinceReset{0};
static std::atomic<CmiUInt8> minSinceReset{0};

static void noteSample(CmiUInt8 bytes) {
  if (bytes == 0)
    return;
  CmiUInt8 seen = maxSinceReset.load(std::memory_order_relaxed);
  while (bytes > seen &&
         !maxSinceReset.compare_exchange_weak(seen, bytes,
                                              std::memory_order_relaxed)) {
  }
  seen = minSinceReset.load(std::memory_order_relaxed);
  while ((seen == 0 || bytes < seen) &&
         !minSinceReset.compare_exchange_weak(seen, bytes,
                                              std::memory_order_relaxed)) {
  }
}

CmiUInt8 CmiMemoryUsage(void) {
  const char *name;
  const CmiUInt8 bytes = probeMemoryUsage(&name);
  noteSample(bytes);
  return bytes;
}

const char *CmiMemoryUsageReporter(void) {
  const char *name;
  probeMemoryUsage(&name);
  return name;
}

CmiUInt8 CmiMaxMemoryUsage(void) {
  CmiMemoryUsage();
  return maxSinceReset.load(std::memory_order_relaxed);
}

void CmiResetMaxMemory(void) {
  const char *name;
  maxSinceReset.store(probeMemoryUsage(&name), std::memory_order_relaxed);
}

CmiUInt8 CmiMinMemoryUsage(void) {
  CmiMemoryUsage();
  return minSinceReset.load(std::memory_order_relaxed);
}

void CmiResetMinMemory(void) {
  const char *name;
  minSinceReset.store(probeMemoryUsage(&name), std::memory_order_relaxed);
}

CmiUInt8 CmiMaxMemoryUsageR(void) {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return (CmiUInt8)pmc.PeakWorkingSetSize;
  return 0;
#else
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0)
    return 0;
#ifdef __APPLE__
  return (CmiUInt8)ru.ru_maxrss; // bytes on macOS
#else
  return (CmiUInt8)ru.ru_maxrss * 1024; // kilobytes on Linux
#endif
#endif
}
