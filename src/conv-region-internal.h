// Regions, counters and attachments: what conv-region.cpp and
// conv-region-attach.cpp share. See doc/internals/persistent-family.md.

#ifndef CONV_REGION_INTERNAL_H
#define CONV_REGION_INTERNAL_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "converse_internal.h"
#include "conv-region.h"

// Design 11's checks that cost something on every contribution. Set by the
// RECONVERSE_ERROR_CHECKING CMake option.
#ifndef CMI_REGION_CHECKS
#define CMI_REGION_CHECKS 0
#endif

struct CmiAttachRec;

namespace cmi_region {

// A PE's counters, and its regions, are numbered below this, the 16-bit
// signal's range.
constexpr uint32_t kMaxCountersPerPe = 65536;
constexpr uint32_t kMaxRegionsPerPe = 65536;

// CmiTarget::flags
enum TargetFlags : uint16_t {
  kTargetHasRegion = 1,
  kTargetHasCounter = 2,
  kTargetQdCounted = 4, // the counter has an action, so QD counts its rounds
  kTargetHasRkey = 8,   // the region is registered with the network
};

// CmiTarget::sigKind: what a put with signal's 16-bit signal names.
enum SignalKind : uint16_t {
  kSignalCounter = 0, // a counter of the owner PE: decrement it
  kSignalRegion = 1,  // a region of the owner PE, for a put without a counter
};

enum LineFlags : uint32_t {
  kLineLive = 1,        // a counter owns the line
  kLineCheckRounds = 2, // CMI_COUNTER_CHECK_ROUNDS
};

// One counter. Everything a contributing thread reads or writes is on this
// line and nothing else is: in phase 3 the line moves into memory that other
// processes map (design 5.1, 5.2).
struct alignas(CMI_CACHE_LINE_SIZE) CounterLine {
  std::atomic<int32_t> word;         // contributions still expected this round
  std::atomic<uint32_t> generation;  // incremented when a destroy completes
  std::atomic<uint32_t> armedRounds; // rounds armed so far (round check)
  std::atomic<uint32_t> flags;       // LineFlags
  int32_t ownerRank; // set when the chunk is allocated, never changed
  uint32_t index;    // likewise
  std::atomic<void *> fireMsg; // the counter's message; null for no action
};
static_assert(std::atomic<int32_t>::is_always_lock_free,
              "the counter word must be lock-free");
static_assert(sizeof(CounterLine) == CMI_CACHE_LINE_SIZE,
              "one counter per cache line");

// What threads other than the owner read of a region slot: the puts without
// a counter that have landed in it, and its generation (the live region's,
// or the next region's once the destroy has completed), which an in-process
// put or get checks against its target's.
struct RegionSlot {
  std::atomic<uint64_t> landed;
  std::atomic<uint32_t> generation;
  uint32_t pad;
};

// A PE's counters, in chunks of a page of lines, allocated by the owner PE
// on first use and never freed. A lookup goes through the chunk table, so
// that phase 3 can place the chunks in a shared area.
struct CounterSlice {
  std::atomic<CounterLine *> *chunks; // kMaxCountersPerPe / linesPerChunk
  // the region slots, in chunks of the same kind
  std::atomic<RegionSlot *> *slotChunks;
};

// The message that runs a counter's action on the owner PE (design 5.3).
struct FireMsg {
  char core[CmiMsgHeaderSizeBytes];
  int32_t rank;
  uint32_t index;
  uint32_t generation;
};

enum class ObjState : int { Live, Destroying };

// Attach-protocol message: registrations or detach notices for objects of
// one owner process, from one origin process (design 10.1). A notice says
// how many contributions the process made, per counter (design 10.2).
enum class AttachOp : uint8_t { Register = 1, Notice = 2 };
enum class ObjKind : uint8_t { Region = 1, Counter = 2 };
struct AttachEntry {
  int32_t ownerPe;
  uint32_t slot; // region slot or counter index
  uint32_t gen;
  uint8_t kind;  // ObjKind
  uint8_t pad[3];
  uint32_t firstCount, nCounts; // this entry's CountEntries in the message
};
// Contributions through one registration: to a counter, or (counterIndex
// kUncounted) puts without a counter into the region.
constexpr uint32_t kUncounted = 0xffffffffu;
struct CountEntry {
  uint32_t counterIndex, counterGen;
  uint64_t count;
};
struct AttachMsg {
  char core[CmiMsgHeaderSizeBytes];
  int32_t fromNode;
  int32_t n, nCounts;
  uint8_t op; // AttachOp
  uint8_t pad[3];
  // n AttachEntries, then nCounts CountEntries
};
inline AttachEntry *attachEntries(AttachMsg *m) {
  return reinterpret_cast<AttachEntry *>(m + 1);
}
inline CountEntry *countEntries(AttachMsg *m) {
  return reinterpret_cast<CountEntry *>(attachEntries(m) + m->n);
}

// Owner to origin process: detach from this object (10.1, 10.2).
struct DetachRequestMsg {
  char core[CmiMsgHeaderSizeBytes];
  int32_t ownerNode;
  AttachEntry entry;
};

struct DoneMsg {
  char core[CmiMsgHeaderSizeBytes];
  CmiDoneFn fn;
  void *arg;
};

// Per-PE state, touched only by its PE.
struct PeState {
  int rank;
  // counters, by index
  std::vector<CmiCounterRec *> counters;
  std::deque<uint32_t> freeCounters; // least recently freed first
  uint32_t nextCounter = 0;
  // each index's message, made for the first counter with an action there
  // and kept for every later one: never freed, so a late push (an
  // erroneous program) cannot reach freed memory
  std::vector<FireMsg *> messages;
  // an index whose destroyed counter's message is still queued; its
  // handler returns the index to freeCounters
  std::vector<uint8_t> withheld;
  // regions, by slot
  std::vector<CmiRegionRec *> regions;
  std::vector<uint32_t> regionGen; // outlives the records
  std::deque<uint32_t> freeRegions;
};

PeState *peState();
CounterLine *lineOf(int rank, uint32_t index); // null if never allocated
// a region slot of a PE of this process; null if never allocated
RegionSlot *regionSlotOf(int rank, uint32_t slot);

// Converse handler indices, the same on every PE.
int attachHandlerIdx();
int detachRequestHandlerIdx();

void postDone(CmiDoneFn fn, void *arg);
void qdCount(int created, int processed);
#if CMI_REGION_CHECKS
// +region_test_completion_delay: spin in operationDone, for tests
void testCompletionDelay();
#endif

// Owner side of the attach protocol (conv-region.cpp): a registration or
// notice, with its counts, for one of this PE's objects; false if the
// object's generation is gone.
bool applyAttach(AttachOp op, const AttachEntry &e, const CountEntry *counts,
                 int fromNode);
void sendDetachRequest(int toNode, const AttachEntry &e);

// Init, called from conv-region-attach.cpp's module init.
void attachNodeInit();
void attachPeInit();

// A transfer to another process through a registration has completed
// locally (origin side), or, for one a message carries, has been sent; the
// last one of a registration whose notice waits sends it.
void operationDone(CmiAttachRec *rec);

// Aborts unless a is a live attachment: names the call, and says whether a
// was detached already.
void checkAttachment(const CmiAttachmentRec *a, const char *call);

} // namespace cmi_region

using cmi_region::CounterLine;
using cmi_region::ObjState;

struct CmiRegionRec {
  struct Piece {
    size_t offset, bytes;
  };
  void *base;
  size_t bytes;
  CmiMemKind kind;
  bool runtimeAllocated;
  int32_t ownerPe;
  uint32_t slot, generation;
  std::vector<Piece> pieces;
  std::unordered_map<int, int> balance; // process: registrations - notices
  std::vector<CmiCounterRec *> pairedCounters;
  comm_backend::mr_t mr; // MR_NULL when not registered
  uint8_t rkey[CMI_TARGET_RKEY_BYTES];
  uint64_t expectedUncounted; // puts without a counter, from the notices
  ObjState state;
  int destroyFlags;
  bool recheck; // a timer will check the destroy again
  CmiDoneFn done;
  void *doneArg;
};

struct CmiCounterRec {
  CounterLine *line;
  int32_t ownerPe;
  uint32_t index, generation;
  CmiCounterAction action;
  int flags;
  int32_t curN;                       // N of the armed round
  uint32_t roundsStarted, roundsArmed;
  // N summed over the rounds armed and over those whose action started; the
  // contributions that have reached the word are sumArmed - word
  uint64_t sumArmed, sumStarted;
  std::vector<CmiRegionRec *> pairedRegions;
  std::unordered_map<int, int> balance; // counter-only attachments
  uint64_t expected; // contributions reported by the notices
  ObjState state;
  int destroyFlags;
  bool recheck; // a timer will check the destroy again
  CmiDoneFn done;
  void *doneArg;
};

// An origin process's registration with one region (or counter) of one
// generation, shared by its attachments (design 3.1, 10.1). Fields other
// than the atomics are under the table lock.
enum class AttachState : int { Registered, DetachedByOwner };
struct CmiAttachRec {
  int32_t ownerNode;
  cmi_region::AttachEntry entry;
  int refs;
  bool inTable;
  bool noticeSent;
  std::atomic<int> state; // AttachState
  // transfers to another process issued and not yet locally complete (one
  // that a message carries: until just after its send), plus
  // kNoticePending once the notice waits for them to reach zero
  std::atomic<uint64_t> outstanding;
  // the PE that sends the notice that waited, and the message that tells it
  // to: set before kNoticePending, and read only by the completion that sees
  // kNoticePending | 1 (operationDone)
  int noticeRank;
  void *noticeMsg;
  std::vector<CmiAttachmentRec *> handles;
  // contributions of detached handles: (counter index, generation) or
  // kUncounted, to count
  std::unordered_map<uint64_t, uint64_t> retired;
};
constexpr uint64_t kNoticePending = 1ull << 63;

// CmiAttachmentRec::magic of a detached handle; a live one's is
// CMI_TARGET_MAGIC.
constexpr uint32_t kAttachmentDetached = 0x44544443u; // "CDTD"
// Detached handles wait in a FIFO and are reused only once more than this
// many wait, so that a use after detach is caught until that many later
// detaches have passed.
constexpr size_t kDetachedHandlesKept = 4096;

// One CmiAttachTarget's handle; a cache line of its own (or more), since
// handles are recycled across PEs and written on every contribution.
struct alignas(CMI_CACHE_LINE_SIZE) CmiAttachmentRec {
  std::atomic<uint32_t> magic; // read by any PE that is handed the handle
  CmiTarget target;
  CmiAttachRec *rec;
  // contributions through this handle; also the round check's k
  std::atomic<uint64_t> contributions;
};

#endif // CONV_REGION_INTERNAL_H
