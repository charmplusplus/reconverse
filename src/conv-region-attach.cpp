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

// This process's registrations, shared by its PEs; only attach, detach and
// a detach request touch it.
CmiNodeLock g_lock;
std::unordered_map<Key, CmiAttachRec *, KeyHash> *g_table = nullptr;

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
  if (t.ownerNode != CmiMyNode())
    CmiAbort("%s: a target of PE %d, in process %d; targets of another "
             "process are not implemented yet\n", call, t.ownerPe,
             t.ownerNode);
  if (t.memKind != CMI_MEM_HOST)
    CmiAbort("%s: device memory is not implemented yet\n", call);
}

// Registrations or notices, all for objects of one owner process, as one
// message to the first entry's PE, which hands the others to their PEs.
// Sent urgent (design 10.2): they must not wait behind application work.
void sendAttachMsg(AttachOp op, int fromNode,
                   const std::vector<AttachEntry> &entries) {
  int n = (int)entries.size();
  int size = (int)(offsetof(AttachMsg, entries) + n * sizeof(AttachEntry));
  auto *m = static_cast<AttachMsg *>(CmiAlloc(size));
  m->fromNode = fromNode;
  m->n = n;
  m->op = (uint8_t)op;
  for (int i = 0; i < n; i++) m->entries[i] = entries[i];
  CmiSetHandler(m, attachHandlerIdx());
  reinterpret_cast<CmiMessageHeader *>(m)->messageSize = size;
  CmiSyncSendAndFreeUrgent(entries[0].ownerPe, size, m);
}

void sendByOwnerNode(AttachOp op,
                     const std::map<int, std::vector<AttachEntry>> &byNode) {
  for (const auto &kv : byNode) sendAttachMsg(op, CmiMyNode(), kv.second);
}

void attachHandler(void *vmsg) {
  auto *m = static_cast<AttachMsg *>(vmsg);
  AttachOp op = (AttachOp)m->op;
  std::map<int, std::vector<AttachEntry>> others;
  for (int i = 0; i < m->n; i++) {
    const AttachEntry &e = m->entries[i];
    if (e.ownerPe == CmiMyPe()) applyAttach(op, e, m->fromNode);
    else others[e.ownerPe].push_back(e);
  }
  for (const auto &kv : others) sendAttachMsg(op, m->fromNode, kv.second);
  CmiFree(m);
}

// In the origin process: the owner destroyed the object, or is destroying
// it with CMI_DESTROY_DETACH_ALL. The registration ends here; attachments
// that still use it see it detached, and the notice goes back at once.
void detachRequestHandler(void *vmsg) {
  auto *m = static_cast<DetachRequestMsg *>(vmsg);
  Key k = keyOf(m->ownerNode, m->entry);
  bool notice = false;
  CmiLock(g_lock);
  auto it = g_table->find(k);
  if (it != g_table->end()) {
    it->second->state.store((int)AttachState::DetachedByOwner,
                            std::memory_order_release);
    g_table->erase(it);
    notice = true;
  }
  CmiUnlock(g_lock);
  if (notice) sendAttachMsg(AttachOp::Notice, CmiMyNode(), {m->entry});
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

void attachNodeInit() {
  g_lock = CmiCreateLock();
  g_table = new std::unordered_map<Key, CmiAttachRec *, KeyHash>();
}

void attachPeInit() {
  CpvInitialize(int, regionAttachHandlerIdx);
  CpvAccess(regionAttachHandlerIdx) = CmiRegisterHandler(attachHandler);
  CpvInitialize(int, regionDetachRequestHandlerIdx);
  CpvAccess(regionDetachRequestHandlerIdx) =
      CmiRegisterHandler(detachRequestHandler);
}

} // namespace cmi_region

int CmiAttachTargets(const CmiTarget *t, int n, CmiAttachment *out) {
  for (int i = 0; i < n; i++) checkTarget(t[i], "CmiAttachTarget");
  std::map<int, std::vector<AttachEntry>> owed; // by owner process
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
      rec->state.store((int)AttachState::Registered, std::memory_order_relaxed);
      owed[t[i].ownerNode].push_back(e);
    }
    rec->refs++;
    auto *a = new CmiAttachmentRec();
    a->magic = CMI_TARGET_MAGIC;
    a->target = t[i];
    a->rec = rec;
    a->seq.store(0, std::memory_order_relaxed);
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
  std::map<int, std::vector<AttachEntry>> notices; // by owner process
  std::vector<CmiAttachRec *> unused;
  CmiLock(g_lock);
  for (int i = 0; i < n; i++) {
    CmiAttachRec *rec = a[i]->rec;
    a[i]->magic = 0;
    delete a[i];
    a[i] = nullptr;
    if (--rec->refs > 0) continue;
    // a registration the owner ended has sent its notice already
    if (rec->state.load(std::memory_order_relaxed) ==
        (int)AttachState::Registered) {
      g_table->erase(keyOf(rec->ownerNode, rec->entry));
      notices[rec->ownerNode].push_back(rec->entry);
    }
    unused.push_back(rec);
  }
  CmiUnlock(g_lock);
  sendByOwnerNode(AttachOp::Notice, notices);
  for (CmiAttachRec *rec : unused) delete rec;
}

void CmiDetachTarget(CmiAttachment a) { CmiDetachTargets(&a, 1); }
