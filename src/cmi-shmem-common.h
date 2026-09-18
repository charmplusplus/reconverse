#ifndef CMI_SHMEM_COMMON_HH
#define CMI_SHMEM_COMMON_HH

#include "converse_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <vector>


namespace cmi {
namespace ipc {
CpvDeclare(std::size_t, kRecommendedCutoff);
}
}  // namespace cmi

CpvStaticDeclare(std::size_t, kSegmentSize);
constexpr std::size_t kDefaultSegmentSize = 8 * 1024 * 1024;

constexpr std::size_t kNumCutOffPoints = 25;
const std::array<std::size_t, kNumCutOffPoints> kCutOffPoints = {
    64,        128,       256,       512,       1024,     2048,     4096,
    8192,      16384,     32768,     65536,     131072,   262144,   524288,
    1048576,   2097152,   4194304,   8388608,   16777216, 33554432, 67108864,
    134217728, 268435456, 536870912, 1073741824};

CpvExtern(int, CthResumeNormalThreadIdx);

CsvStaticDeclare(CmiNodeLock, sleeper_lock);

using sleeper_map_t = std::vector<CthThread>;
CsvStaticDeclare(sleeper_map_t, sleepers);

// the data each pe shares with its peers
// contains pool of free blocks, heap, and receive queue
struct ipc_shared_ {
  std::array<std::atomic<std::uintptr_t>, kNumCutOffPoints> free;
  std::atomic<std::uintptr_t> queue;
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;

  ipc_shared_(std::uintptr_t begin, std::uintptr_t end)
      : queue(cmi::ipc::max), heap(cmi::ipc::nil), max(end) {
    for (auto& f : this->free) {
      f.store(cmi::ipc::max);
    }
    // publish the heap last -- remote peers treat (heap == nil) as "not
    // ready" (timeout), so this release-store is what makes the segment
    // visible for allocation with all other fields initialized
    this->heap.store(begin, std::memory_order_release);
  }
};

// Shared state for one IPC pool, common to every backend. Backends derive
// from this and add whatever they need to map a peer's segment (file
// descriptors for POSIX shared memory, segment/access ids for xpmem); the
// block allocator in cmishmem.cpp only ever touches the fields here, so it
// is the same code whichever backend mapped the segments.
struct CmiIpcManager {
  // maps procs to shared segments; pre-sized so lookups never mutate the
  // container (PE threads poll this concurrently with segment attachment)
  std::vector<std::atomic<ipc_shared_*>> shared;
  // physical node rank
  int mine;
  // key of this instance
  std::size_t key;
  // set once every peer segment on this host has been mapped; until then
  // allocation reports CMI_IPC_TIMEOUT and senders fall back to the network
  std::atomic<bool> ready;
  // peers[node] is nonzero for every *other* process sharing this host, so
  // the send path can rule out a network destination with one load
  std::vector<char> peers;
  // number of processes (including this one) on this host
  int nPeers;
  // base constructor
  CmiIpcManager(std::size_t key_)
      : shared(CmiNumNodes()),
        mine(CmiMyNode()),
        key(key_),
        ready(false),
        peers(CmiNumNodes(), 0),
        nPeers(1) {}
  // virtual destructor may be needed
  virtual ~CmiIpcManager() {}
};

// Which mechanism maps one process's pool into its peers' address spaces.
enum CmiIpcMode {
  CMI_IPC_MODE_OFF = 0,
  CMI_IPC_MODE_POSIX_SHM,
  CMI_IPC_MODE_XPMEM
};

inline std::size_t whichBin_(std::size_t size) {
  const auto* begin = kCutOffPoints.data();
  const auto* end = kCutOffPoints.data() + kNumCutOffPoints;
  const auto* it = std::lower_bound(begin, end, size);
  return static_cast<std::size_t>(it - begin);  // kNumCutOffPoints if none
}

inline static void initIpcShared_(ipc_shared_* shared) {
  auto begin = (std::uintptr_t)(sizeof(ipc_shared_) +
                                (sizeof(ipc_shared_) % ALIGN_BYTES));
  CmiAssert(begin != cmi::ipc::nil);
  auto end = begin + CpvAccess(kSegmentSize);
  new (shared) ipc_shared_(begin, end);
}

inline static ipc_shared_* makeIpcShared_(void) {
  auto* shared = (ipc_shared_*)(::operator new(sizeof(ipc_shared_) +
                                               CpvAccess(kSegmentSize)));
  initIpcShared_(shared);
  return shared;
}

inline void initSegmentSize_(char** argv) {
  using namespace cmi::ipc;
  CpvInitialize(std::size_t, kRecommendedCutoff);
  CpvInitialize(std::size_t, kSegmentSize);

  CmiInt8 value;
  auto flag =
      CmiGetArgLongDesc(argv, "++" CMI_IPC_POOL_SIZE_ARG, &value, CMI_IPC_POOL_SIZE_DESC);
  CpvAccess(kSegmentSize) = flag ? (std::size_t)value : kDefaultSegmentSize;
  CmiEnforceMsg(CpvAccess(kSegmentSize), "segment size must be non-zero!");
  if (CmiGetArgLongDesc(argv, "++" CMI_IPC_CUTOFF_ARG, &value, CMI_IPC_CUTOFF_DESC)) {
    auto bin = whichBin_((std::size_t)value);
    CmiEnforceMsg(bin < kNumCutOffPoints, "ipc cutoff out of range!");
    CpvAccess(kRecommendedCutoff) = kCutOffPoints[bin];
  } else {
    auto max = CpvAccess(kSegmentSize) / kNumCutOffPoints;
    auto bin = (std::intptr_t)whichBin_(max) - 1;
    CpvAccess(kRecommendedCutoff) = kCutOffPoints[(bin >= 0) ? bin : 0];
  }
}

inline static void printIpcStartupMessage_(const char* implName) {
  using namespace cmi::ipc;
  CmiPrintf("Converse> %s pool init'd with %luB segment and %luB cutoff.\n",
            implName, CpvAccess(kSegmentSize),
            CpvAccess(kRecommendedCutoff));
}

inline static void initSleepers_(void) {
  if (CmiMyRank() == 0) {
    CsvInitialize(sleeper_map_t, sleepers);
    CsvAccess(sleepers).resize(CmiMyNodeSize());
    CsvInitialize(CmiNodeLock, sleeper_lock);
    CsvAccess(sleeper_lock) = CmiCreateLock();
  }
}

inline static void putSleeper_(CthThread th) {
  CmiLock(CsvAccess(sleeper_lock));
  (CsvAccess(sleepers))[CmiMyRank()] = th;
  CmiUnlock(CsvAccess(sleeper_lock));
}

static void awakenSleepers_(void);
// records which peer processes share this host, and marks the pool usable
static void finishSetup_(CmiIpcManager* meta);

using ipc_manager_ptr_ = std::unique_ptr<CmiIpcManager>;
using ipc_manager_map_ = std::vector<ipc_manager_ptr_>;
CsvStaticDeclare(ipc_manager_map_, managers_);

#endif
