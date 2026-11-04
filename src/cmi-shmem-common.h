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

// Where the pool stops paying for itself. Carrying a message costs one memcpy
// into the destination's segment, which grows with the message, while the
// backend hands large messages to the NIC without a CPU copy at all; past
// some size the copy is dearer than the backend's fixed overhead. On Delta
// over LCI/Slingshot-11 the pool still wins 2.4x at 32 KiB and has lost by
// 128 KiB (see the table in the README), so this is the largest size measured
// to be worth taking. It is deliberately conservative: a machine whose
// crossover is higher gives up some of the win until ++ipccutoff raises it.
constexpr std::size_t kDefaultCutoff = 32 * 1024;

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

// Bounds of the per-PE structures in a segment. A process has at most
// kMaxIpcRanks PEs, and a host at most kMaxIpcHostPes PEs over all its
// processes; a run past either stops at start-up with an error
// (finishSetup_).
constexpr int kMaxIpcRanks = 256;
constexpr int kMaxIpcHostPes = 512;
// Block sizes the per-sender return lists cover: kCutOffPoints[0..15], up to
// 2 MiB. Sixteen 8-byte heads fill the two cache lines one sender's lists
// take anyway, so covering fewer would save no space. ++ipccutoff is capped
// so that a message at the cutoff, with its headers, still fits the largest
// of them (initSegmentSize_).
constexpr int kIpcRetBins = 16;
static_assert(kIpcRetBins <= (int)kNumCutOffPoints, "more return bins than block sizes");

// One list head per cache line: every list below is written by PEs of
// several processes, and two on one line would make each writer's atomic
// operations fight over it.
struct alignas(64) ipc_list_ {
  std::atomic<std::uintptr_t> head;
};

// One sender's lists of empty blocks in one segment, a list per block size.
struct alignas(64) ipc_return_ {
  std::array<std::atomic<std::uintptr_t>, kIpcRetBins> bin;
};

// The header of one process's segment. Each process creates one segment and
// every other process on the host maps it. A message sent to this process is
// copied into a block in this segment, and the receiving PE runs it in place.
//
// - heap: offset of the segment's unused space. A sender that has no empty
//   block of the size it needs carves a new one here (compare-and-swap; the
//   offset moves only if the block fits).
// - rq[rank]: full blocks, holding messages for that PE of this process.
//   Senders push; only that PE takes from it.
// - node_rq: full blocks holding node-level messages (CmiSyncNodeSend*). Any
//   PE of this process may take from it.
// - ret[sender].bin[b]: empty blocks of size b. They are the blocks that PE
//   `sender` (indexed among all the host's PEs) carved from this segment;
//   this process's PEs push each one back here when they free the message
//   in it. Only `sender` takes from it, and reuses the blocks for its next
//   messages to this process. A block therefore always goes back to the PE
//   that carved it, never to another sender.
//
// Each list is a linked stack of segment offsets, ended by cmi::ipc::max.
// Any number of PEs push with a compare-and-swap, and holds no lock while
// doing so. A taker never removes a single block: it exchanges the head for
// "empty" and gets every block on the list at once. Since no other PE can
// reach those blocks after that, no PE can reuse a block while the taker
// follows its next pointer, which is what makes the lists safe without a
// lock.
//
// (Earlier, one receive queue per process and one free list per block size
// served every sender and receiver on the host, and each list's head doubled
// as its lock; at ~3 M small messages/s per node over 2 x 23 PEs, a send
// took 16 us.)
struct ipc_shared_ {
  std::atomic<std::uintptr_t> heap;
  std::uintptr_t max;
  ipc_list_ node_rq;
  std::array<ipc_list_, kMaxIpcRanks> rq;
  std::array<ipc_return_, kMaxIpcHostPes> ret;

  ipc_shared_(std::uintptr_t begin, std::uintptr_t end)
      : heap(cmi::ipc::nil), max(end) {
    node_rq.head.store(cmi::ipc::max, std::memory_order_relaxed);
    for (auto& q : rq) q.head.store(cmi::ipc::max, std::memory_order_relaxed);
    for (auto& r : ret)
      for (auto& b : r.bin) b.store(cmi::ipc::max, std::memory_order_relaxed);
    // publish the heap last -- remote peers treat (heap == nil) as "not
    // ready" (timeout), so this release-store is what makes the segment
    // visible for allocation with all other fields initialized
    this->heap.store(begin, std::memory_order_release);
  }
};

// What one PE keeps in its own memory, touched by no other thread:
// - hostIdx: its index among the host's PEs, which names its ret[] lists.
// - cache[proc][b]: empty blocks of size b in process proc's segment, ready
//   for its next sends there. When it runs out, the PE refills it by taking
//   its whole ret[hostIdx].bin[b] list in that segment.
// - inbox: full blocks it took from its receive queue and has not delivered
//   yet, oldest last.
struct ipc_pe_state_ {
  int hostIdx = -1;
  std::vector<std::array<std::uintptr_t, kIpcRetBins>> cache;
  std::vector<CmiIpcBlock*> inbox;
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
  // per rank of this process, written only by that rank (after setup)
  std::vector<ipc_pe_state_> pe;
  // base constructor
  CmiIpcManager(std::size_t key_)
      : shared(CmiNumNodes()),
        mine(CmiMyNode()),
        key(key_),
        ready(false),
        peers(CmiNumNodes(), 0),
        nPeers(1),
        pe(CmiMyNodeSize()) {}
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
    // A block holds the message and its headers, so a cutoff in the largest
    // bin the return lists cover would put messages at the cutoff past it.
    constexpr std::size_t maxBin = kIpcRetBins - 2;
    if (bin > maxBin) {
      if (CmiMyPe() == 0) {
        CmiPrintf("Converse> ++ipccutoff %ld is above the pool's largest, %zu; "
                  "using %zu.\n", (long)value, kCutOffPoints[maxBin],
                  kCutOffPoints[maxBin]);
      }
      bin = maxBin;
    }
    CpvAccess(kRecommendedCutoff) = kCutOffPoints[bin];
  } else {
    // Two ceilings, and the default is the lower. One is capacity: a message
    // the pool cannot hold several of would starve every other peer, so keep
    // it under a bin of segment/25. The other is kDefaultCutoff, which is
    // about speed and does not move with the pool's size. A pool shrunk with
    // ++ipcpoolsize hits the capacity ceiling first and keeps its old value.
    auto capacity = CpvAccess(kSegmentSize) / kNumCutOffPoints;
    auto bin = (std::intptr_t)whichBin_(capacity) - 1;
    auto byCapacity = kCutOffPoints[(bin >= 0) ? bin : 0];
    CpvAccess(kRecommendedCutoff) = std::min(byCapacity, kDefaultCutoff);
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
