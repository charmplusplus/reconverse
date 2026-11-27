// Attachments: an origin process's registrations with the owners of the
// regions and counters it uses, and the messages of the attach protocol
// (design 10.1, 10.2). See doc/internals/persistent-family.md.

#include <cstddef>
#include <map>

#include "conv-region-internal.h"

using namespace cmi_region;

namespace {

struct Key {
  int32_t ownerNode, ownerPe;
  uint32_t slot, gen;
  uint8_t kind;
  bool operator==(const Key &o) const {
    return ownerNode == o.ownerNode && ownerPe == o.ownerPe &&
           slot == o.slot && gen == o.gen && kind == o.kind;
  }
};
struct KeyHash {
  size_t operator()(const Key &k) const {
    uint64_t h = ((uint64_t)(uint32_t)k.ownerPe << 32) ^ k.slot;
    h = h * 0x9e3779b97f4a7c15ull ^ ((uint64_t)k.gen << 8) ^ k.kind;
    return (size_t)(h ^ (h >> 29) ^ (uint64_t)(uint32_t)k.ownerNode);
  }
};

// This process's registrations, shared by its PEs; attach, detach, a detach
// request and a notice that waited touch it, never a put.
CmiNodeLock g_lock;
std::unordered_map<Key, CmiAttachRec *, KeyHash> *g_table = nullptr;
// Detached handles, kept for later attachments and never freed, so that the
// check of a handle detached already reads memory that is still ours.
std::vector<CmiAttachmentRec *> *g_freeHandles = nullptr;

Key keyOf(int ownerNode, const AttachEntry &e) {
  return Key{ownerNode, e.ownerPe, e.slot, e.gen, e.kind};
}

AttachEntry entryOf(const CmiTarget &t) {
  AttachEntry e{};
  e.ownerPe = t.ownerPe;
  if (t.flags & kTargetHasRegion) {
    e.kind = (uint8_t)ObjKind::Region;
    e.slot = t.regionSlot;
    e.gen = t.regionGen;
  } else {
    e.kind = (uint8_t)ObjKind::Counter;
    e.slot = t.counterIndex;
    e.gen = t.counterGen;
  }
  return e;
}

void checkTarget(const CmiTarget &t, const char *call) {
  if (t.magic != CMI_TARGET_MAGIC)
    CmiAbort("%s: not a target made by CmiTargetMake\n", call);
  if (t.ownerNode < 0 || t.ownerNode >= CmiNumNodes())
    CmiAbort("%s: a target of process %d, which is not in this job\n", call,
             t.ownerNode);
  if (t.memKind != CMI_MEM_HOST)
    CmiAbort("%s: device memory is not implemented yet\n", call);
}

// One registration's entry, with its counts for a notice.
struct Pending {
  AttachEntry entry;
  std::vector<CountEntry> counts;
};

uint64_t countKey(uint32_t index, uint32_t gen) {
  return ((uint64_t)gen << 32) | index;
}

// What the process contributed through a registration, for its notice
// (design 10.2): every contribution to a counter, and, for an owner in
// another process, the puts without a counter (one within a process has
// landed when CmiPut returns). Under the table lock.
std::vector<CountEntry> countsOf(CmiAttachRec *rec) {
  std::unordered_map<uint64_t, uint64_t> all = rec->retired;
  for (CmiAttachmentRec *a : rec->handles) {
    uint64_t n = a->contributions.load(std::memory_order_relaxed);
    if (n == 0) continue;
    const CmiTarget &t = a->target;
    if (t.flags & kTargetHasCounter) all[countKey(t.counterIndex, t.counterGen)] += n;
    else all[kUncounted] += n;
  }
  bool local = rec->ownerNode == CmiMyNode();
  std::vector<CountEntry> v;
  for (const auto &kv : all) {
    if (kv.first == kUncounted && local) continue;
    CountEntry c{};
    c.counterIndex = kv.first == kUncounted ? kUncounted : (uint32_t)kv.first;
    c.counterGen = kv.first == kUncounted ? 0 : (uint32_t)(kv.first >> 32);
    c.count = kv.second;
    v.push_back(c);
  }
  return v;
}

// A handle goes: its contributions stay with the registration. Under the
// table lock.
void retire(CmiAttachRec *rec, CmiAttachmentRec *a) {
  uint64_t n = a->contributions.load(std::memory_order_relaxed);
  if (n != 0) {
    const CmiTarget &t = a->target;
    uint64_t k = (t.flags & kTargetHasCounter)
                     ? countKey(t.counterIndex, t.counterGen)
                     : kUncounted;
    rec->retired[k] += n;
  }
  auto &h = rec->handles;
  for (size_t i = 0; i < h.size(); i++)
    if (h[i] == a) {
      h[i] = h.back();
      h.pop_back();
      break;
    }
}

// Registrations or notices, all for objects of one owner process, as one
// message to the first entry's PE, which hands the others to their PEs.
// Sent urgent (design 10.2): they must not wait behind application work.
void sendAttachMsg(AttachOp op, int fromNode, const std::vector<Pending> &items) {
  int n = (int)items.size(), nCounts = 0;
  for (const Pending &p : items) nCounts += (int)p.counts.size();
  int size = (int)(sizeof(AttachMsg) + n * sizeof(AttachEntry) +
                   nCounts * sizeof(CountEntry));
  auto *m = static_cast<AttachMsg *>(CmiAlloc(size));
  m->fromNode = fromNode;
  m->n = n;
  m->nCounts = nCounts;
  m->op = (uint8_t)op;
  AttachEntry *e = attachEntries(m);
  CountEntry *c = countEntries(m);
  int k = 0;
  for (int i = 0; i < n; i++) {
    e[i] = items[i].entry;
    e[i].firstCount = (uint32_t)k;
    e[i].nCounts = (uint32_t)items[i].counts.size();
    for (const CountEntry &ce : items[i].counts) c[k++] = ce;
  }
  CmiSetHandler(m, attachHandlerIdx());
  reinterpret_cast<CmiMessageHeader *>(m)->messageSize = size;
  CmiSyncSendAndFreeUrgent(items[0].entry.ownerPe, size, m);
}

void sendByOwnerNode(AttachOp op,
                     const std::map<int, std::vector<Pending>> &byNode) {
  for (const auto &kv : byNode) sendAttachMsg(op, CmiMyNode(), kv.second);
}

void attachHandler(void *vmsg) {
  auto *m = static_cast<AttachMsg *>(vmsg);
  AttachOp op = (AttachOp)m->op;
  AttachEntry *e = attachEntries(m);
  CountEntry *c = countEntries(m);
  std::map<int, std::vector<Pending>> others;
  for (int i = 0; i < m->n; i++) {
    if (e[i].ownerPe == CmiMyPe()) {
      applyAttach(op, e[i], c + e[i].firstCount, m->fromNode);
    } else {
      Pending p{e[i], std::vector<CountEntry>(c + e[i].firstCount,
                                              c + e[i].firstCount + e[i].nCounts)};
      others[e[i].ownerPe].push_back(p);
    }
  }
  for (const auto &kv : others) sendAttachMsg(op, m->fromNode, kv.second);
  CmiFree(m);
}

// A registration owes its notice (it is out of the table): send it now if no
// network operation through it is in flight, else leave it to the last one
// to complete (operationDone). Under the table lock; returns true and fills
// p when the notice is to be sent now.
bool oweNotice(CmiAttachRec *rec, Pending &p);

// The message operationDone queues on the PE that owes a notice.
struct NoticeMsg {
  char core[CmiMsgHeaderSizeBytes];
  CmiAttachRec *rec;
};

CpvStaticDeclare(int, regionNoticeHandlerIdx);

bool oweNotice(CmiAttachRec *rec, Pending &p) {
  if (rec->noticeMsg == nullptr) {
    auto *m = static_cast<NoticeMsg *>(CmiAlloc(sizeof(NoticeMsg)));
    m->rec = rec;
    CmiSetHandler(m, CpvAccess(regionNoticeHandlerIdx));
    reinterpret_cast<CmiMessageHeader *>(m)->messageSize = sizeof(NoticeMsg);
    rec->noticeMsg = m;
  }
  rec->noticeRank = CmiMyRank();
  // seq_cst, against a transfer's count and state check (beginRemote)
  uint64_t v = rec->outstanding.fetch_or(kNoticePending, std::memory_order_seq_cst);
  if ((v & ~kNoticePending) != 0) return false; // the last completion sends it
  p.entry = rec->entry;
  p.counts = countsOf(rec);
  rec->noticeSent = true;
  return true;
}

void freeRec(CmiAttachRec *rec) {
  if (rec->noticeMsg != nullptr) CmiFree(rec->noticeMsg);
  delete rec;
}

// On the PE that owes a notice, once its registration's operations are done.
void noticeHandler(void *vmsg) {
  auto *m = static_cast<NoticeMsg *>(vmsg);
  CmiAttachRec *rec = m->rec;
  std::vector<Pending> items(1);
  bool del;
  CmiLock(g_lock);
  items[0].entry = rec->entry;
  items[0].counts = countsOf(rec);
  rec->noticeSent = true;
  del = rec->refs == 0;
  if (del) rec->noticeMsg = nullptr; // this message; freed below
  CmiUnlock(g_lock);
  sendAttachMsg(AttachOp::Notice, CmiMyNode(), items);
  if (del) {
    delete rec;
    CmiFree(m);
  }
}

// In the origin process: the owner destroyed the object, or is destroying
// it with CMI_DESTROY_DETACH_ALL. The registration ends here; attachments
// that still use it see it detached, and the notice goes back once its
// operations in flight have completed.
void detachRequestHandler(void *vmsg) {
  auto *m = static_cast<DetachRequestMsg *>(vmsg);
  Key k = keyOf(m->ownerNode, m->entry);
  std::vector<Pending> now;
  CmiLock(g_lock);
  auto it = g_table->find(k);
  if (it != g_table->end()) {
    CmiAttachRec *rec = it->second;
    // seq_cst, against a transfer's count and state check (beginRemote)
    rec->state.store((int)AttachState::DetachedByOwner, std::memory_order_seq_cst);
    g_table->erase(it);
    rec->inTable = false;
    Pending p;
    if (oweNotice(rec, p)) now.push_back(p);
  }
  CmiUnlock(g_lock);
  if (!now.empty()) sendAttachMsg(AttachOp::Notice, CmiMyNode(), now);
  CmiFree(m);
}

} // namespace

CpvStaticDeclare(int, regionAttachHandlerIdx);
CpvStaticDeclare(int, regionDetachRequestHandlerIdx);

namespace cmi_region {

int attachHandlerIdx() { return CpvAccess(regionAttachHandlerIdx); }
int detachRequestHandlerIdx() {
  return CpvAccess(regionDetachRequestHandlerIdx);
}

void sendDetachRequest(int toNode, const AttachEntry &e) {
  auto *m = static_cast<DetachRequestMsg *>(CmiAlloc(sizeof(DetachRequestMsg)));
  m->ownerNode = CmiMyNode();
  m->entry = e;
  CmiSetHandler(m, detachRequestHandlerIdx());
  reinterpret_cast<CmiMessageHeader *>(m)->messageSize = sizeof(DetachRequestMsg);
  CmiSyncSendAndFreeUrgent(CmiNodeFirst(toNode), sizeof(DetachRequestMsg), m);
}

// The completion that takes the count to zero, with the notice waiting, is
// the only one that sees kNoticePending | 1: it hands the notice to the PE
// that owes it. It reads that PE and the message only after its fetch_sub,
// and only then: oweNotice sets them before its fetch_or, and the fetch_sub
// that reads the bit acquires what the fetch_or released. The registration
// is still allocated then, since its notice has not been sent and only
// noticeHandler, which this push runs, frees it. A completion that sees any
// other value no longer touches the registration: a later completion, or
// the detach itself, may free it at any time after the fetch_sub.
void operationDone(CmiAttachRec *rec) {
#if CMI_REGION_CHECKS
  testCompletionDelay();
#endif
  if (rec->outstanding.fetch_sub(1, std::memory_order_acq_rel) ==
      (kNoticePending | 1))
    CmiPushPE(rec->noticeRank, rec->noticeMsg);
}

void attachNodeInit() {
  g_lock = CmiCreateLock();
  g_table = new std::unordered_map<Key, CmiAttachRec *, KeyHash>();
  g_freeHandles = new std::vector<CmiAttachmentRec *>();
}

void attachPeInit() {
  CpvInitialize(int, regionAttachHandlerIdx);
  CpvAccess(regionAttachHandlerIdx) = CmiRegisterHandler(attachHandler);
  CpvInitialize(int, regionDetachRequestHandlerIdx);
  CpvAccess(regionDetachRequestHandlerIdx) =
      CmiRegisterHandler(detachRequestHandler);
  CpvInitialize(int, regionNoticeHandlerIdx);
  CpvAccess(regionNoticeHandlerIdx) = CmiRegisterHandler(noticeHandler);
}

} // namespace cmi_region

int CmiAttachTargets(const CmiTarget *t, int n, CmiAttachment *out) {
  for (int i = 0; i < n; i++) checkTarget(t[i], "CmiAttachTarget");
  std::map<int, std::vector<Pending>> owed; // by owner process
  CmiLock(g_lock);
  for (int i = 0; i < n; i++) {
    AttachEntry e = entryOf(t[i]);
    Key k = keyOf(t[i].ownerNode, e);
    CmiAttachRec *&rec = (*g_table)[k];
    if (rec == nullptr) {
      rec = new CmiAttachRec();
      rec->ownerNode = t[i].ownerNode;
      rec->entry = e;
      rec->refs = 0;
      rec->inTable = true;
      rec->noticeSent = false;
      rec->state.store((int)AttachState::Registered, std::memory_order_relaxed);
      rec->outstanding.store(0, std::memory_order_relaxed);
      rec->noticeRank = -1;
      rec->noticeMsg = nullptr;
      owed[t[i].ownerNode].push_back(Pending{e, {}});
    }
    rec->refs++;
    CmiAttachmentRec *a;
    if (g_freeHandles->empty()) {
      a = new CmiAttachmentRec();
    } else {
      a = g_freeHandles->back();
      g_freeHandles->pop_back();
    }
    a->magic = CMI_TARGET_MAGIC;
    a->target = t[i];
    a->rec = rec;
    a->contributions.store(0, std::memory_order_relaxed);
    rec->handles.push_back(a);
    out[i] = a;
  }
  CmiUnlock(g_lock);
  sendByOwnerNode(AttachOp::Register, owed);
  return n;
}

CmiAttachment CmiAttachTarget(const CmiTarget *t) {
  CmiAttachment a = nullptr;
  CmiAttachTargets(t, 1, &a);
  return a;
}

void CmiDetachTargets(CmiAttachment *a, int n) {
  for (int i = 0; i < n; i++)
    if (a[i] == nullptr || a[i]->magic != CMI_TARGET_MAGIC)
      CmiAbort("CmiDetachTarget: not an attachment from CmiAttachTarget, or "
               "one already detached\n");
  std::map<int, std::vector<Pending>> notices; // by owner process
  std::vector<CmiAttachRec *> unused;
  CmiLock(g_lock);
  for (int i = 0; i < n; i++) {
    CmiAttachRec *rec = a[i]->rec;
    retire(rec, a[i]);
    a[i]->magic = 0;
    g_freeHandles->push_back(a[i]);
    a[i] = nullptr;
    if (--rec->refs > 0) continue;
    if (rec->inTable) {
      g_table->erase(keyOf(rec->ownerNode, rec->entry));
      rec->inTable = false;
      Pending p;
      if (oweNotice(rec, p)) notices[rec->ownerNode].push_back(p);
    }
    // a registration whose notice waits is freed by noticeHandler
    if (rec->noticeSent) unused.push_back(rec);
  }
  CmiUnlock(g_lock);
  sendByOwnerNode(AttachOp::Notice, notices);
  for (CmiAttachRec *rec : unused) freeRec(rec);
}

void CmiDetachTarget(CmiAttachment a) { CmiDetachTargets(&a, 1); }
