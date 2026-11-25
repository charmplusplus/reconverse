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

namespace cmi_region {

// A PE's counters are numbered below this, the 16-bit signal's range.
constexpr uint32_t kMaxCountersPerPe = 65536;

// CmiTarget::flags
enum TargetFlags : uint16_t {
  kTargetHasRegion = 1,
  kTargetHasCounter = 2,
  kTargetQdCounted = 4, // the counter has an action, so QD counts its rounds
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
  int32_t ownerRank;
  uint32_t index;
  std::atomic<void *> fireMsg; // the counter's message; null for no action
};
static_assert(std::atomic<int32_t>::is_always_lock_free,
              "the counter word must be lock-free");
static_assert(sizeof(CounterLine) == CMI_CACHE_LINE_SIZE,
              "one counter per cache line");

// A PE's counters, in chunks of a page of lines, allocated by the owner PE
// on first use and never freed. A lookup goes through the chunk table, so
// that phase 3 can place the chunks in a shared area.
struct CounterSlice {
  std::atomic<CounterLine *> *chunks; // kMaxCountersPerPe / linesPerChunk
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
// one owner process, from one origin process (design 10.1).
enum class AttachOp : uint8_t { Register = 1, Notice = 2 };
enum class ObjKind : uint8_t { Region = 1, Counter = 2 };
struct AttachEntry {
  int32_t ownerPe;
  uint32_t slot; // region slot or counter index
  uint32_t gen;
  uint8_t kind;  // ObjKind
  uint8_t pad[3];
};
struct AttachMsg {
  char core[CmiMsgHeaderSizeBytes];
  int32_t fromNode;
  int32_t n;
  uint8_t op; // AttachOp
  uint8_t pad[7];
  AttachEntry entries[1]; // n of them
};

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
  // regions, by slot
  std::vector<CmiRegionRec *> regions;
  std::vector<uint32_t> regionGen; // outlives the records
  std::deque<uint32_t> freeRegions;
};

PeState *peState();
CounterLine *lineOf(int rank, uint32_t index); // null if never allocated

// Converse handler indices, the same on every PE.
int attachHandlerIdx();
int detachRequestHandlerIdx();

void postDone(CmiDoneFn fn, void *arg);
void qdCount(int created, int processed);

// Owner side of the attach protocol (conv-region.cpp): a registration or
// notice for one of this PE's objects; false if the object's generation is
// gone.
bool applyAttach(AttachOp op, const AttachEntry &e, int fromNode);
void sendDetachRequest(int toNode, const AttachEntry &e);

// Init, called from conv-region-attach.cpp's module init.
void attachNodeInit();
void attachPeInit();

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
  ObjState state;
  int destroyFlags;
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
  ObjState state;
  int destroyFlags;
  CmiDoneFn done;
  void *doneArg;
};

// An origin process's registration with one region (or counter) of one
// generation, shared by its attachments (design 3.1, 10.1).
enum class AttachState : int { Registered, DetachedByOwner };
struct CmiAttachRec {
  int32_t ownerNode;
  cmi_region::AttachEntry entry;
  int refs; // under the table lock
  std::atomic<int> state; // AttachState
};

// One CmiAttachTarget's handle.
struct CmiAttachmentRec {
  uint32_t magic;
  CmiTarget target;
  CmiAttachRec *rec;
  std::atomic<uint32_t> seq; // contributions so far (round check)
};

#endif // CONV_REGION_INTERNAL_H
