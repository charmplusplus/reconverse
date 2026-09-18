// XPMEM backend for the IPC pool.
//
// Included by cmishmem.cpp (not compiled on its own), and only when the build
// found xpmem. Where the POSIX shm backend puts the pool in a file both
// processes map, this one keeps the pool in ordinary heap memory and lets
// peers attach to it directly: each process exports its address space with
// xpmem_make and broadcasts the resulting segment id, and each peer turns
// that id plus the pool's address into a local mapping with xpmem_get and
// xpmem_attach.
//
// Whether that works at run time depends on the xpmem kernel module being
// loaded, which the library cannot tell us before /dev/xpmem is opened. See
// ipcXpmemUsable_() -- cmishmem.cpp falls back to POSIX shm when it says no.
#include <fcntl.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <vector>

extern "C" {
#include <xpmem.h>
}

// "borrowed" from VADER
// (https://github.com/open-mpi/ompi/tree/386ba164557bb8115131921041757be94a989646/opal/mca/smsc/xpmem)
#define OPAL_DOWN_ALIGN(x, a, t) ((x) & ~(((t)(a)-1)))
#define OPAL_DOWN_ALIGN_PTR(x, a, t) \
  ((t)OPAL_DOWN_ALIGN((uintptr_t)x, a, uintptr_t))
#define OPAL_ALIGN(x, a, t) (((x) + ((t)(a)-1)) & ~(((t)(a)-1)))
#define OPAL_ALIGN_PTR(x, a, t) ((t)OPAL_ALIGN((uintptr_t)x, a, uintptr_t))
#define OPAL_ALIGN_PAD_AMOUNT(x, s) \
  ((~((uintptr_t)(x)) + 1) & ((uintptr_t)(s)-1))

CpvStaticDeclare(int, handle_init);

struct init_msg_ {
  char core[CmiMsgHeaderSizeBytes];
  std::size_t key;
  int from;
  xpmem_segid_t segid;
  ipc_shared_* shared;
  // Size of the exporter's pool. The importer has to attach the whole pool,
  // not just its header: a block handed back by the allocator can sit
  // anywhere in it, and an attachment that only covers sizeof(ipc_shared_)
  // faults the moment a peer writes past the header.
  std::size_t span;
};

// Reports whether xpmem can actually be used in this run. The library links
// fine on any Cray system, but xpmem_make fails with ENOENT unless the kernel
// module is loaded and /dev/xpmem exists, which is a per-host property that
// only shows up at run time.
static bool ipcXpmemUsable_(void) {
  int fd = open("/dev/xpmem", O_RDWR);
  if (fd < 0) return false;
  close(fd);
  return true;
}

// NOTE ( we should eventually detach xpmem segments at close )
//      ( it's not urgently needed since xpmem does it for us )
struct ipcManagerXpmem_ : public CmiIpcManager {
  // maps ranks to segments
  std::map<int, xpmem_segid_t> segments;
  // maps segments to xpmem apids
  std::map<xpmem_segid_t, xpmem_apid_t> instances;
  // size of this process's own pool, sent to peers so they attach all of it
  std::size_t span;
  // create our local shared data
  ipcManagerXpmem_(std::size_t key) : CmiIpcManager(key) {
    this->span = sizeof(ipc_shared_) + CpvAccess(kSegmentSize);
    this->shared[this->mine].store(makeIpcShared_(), std::memory_order_release);
  }

  void put_segment(int proc, const xpmem_segid_t& segid) {
    auto ins = this->segments.emplace(proc, segid);
    CmiAssert(ins.second);
  }

  xpmem_segid_t get_segment(int proc) {
    auto search = this->segments.find(proc);
    if (search == std::end(this->segments)) {
      if (mine == proc) {
        auto segid =
            xpmem_make(0, XPMEM_MAXADDR_SIZE, XPMEM_PERMIT_MODE, (void*)0666);
        CmiEnforceMsg(segid >= 0,
                      "xpmem_make failed -- is the xpmem kernel module "
                      "loaded on this host?");
        this->put_segment(mine, segid);
        return segid;
      } else {
        return -1;
      }
    } else {
      return search->second;
    }
  }

  xpmem_apid_t get_instance(int proc) {
    auto segid = this->get_segment(proc);
    if (segid >= 0) {
      auto search = this->instances.find(segid);
      if (search == std::end(this->instances)) {
        auto apid = xpmem_get(segid, XPMEM_RDWR, XPMEM_PERMIT_MODE, NULL);
        CmiAssertMsg(apid >= 0, "invalid segid?");
        auto ins = this->instances.emplace(segid, apid);
        CmiAssert(ins.second);
        search = ins.first;
      }
      return search->second;
    } else {
      return -1;
    }
  }
};

static void* translateAddr_(ipcManagerXpmem_* meta, int proc, void* remote_ptr,
                            const std::size_t& size) {
  if (proc == meta->mine) {
    return remote_ptr;
  } else {
    auto apid = meta->get_instance(proc);
    CmiAssert(apid >= 0);
    // this magic was borrowed from VADER
    uintptr_t attach_align = 1 << 23;
    auto base = OPAL_DOWN_ALIGN_PTR(remote_ptr, attach_align, uintptr_t);
    auto bound =
        OPAL_ALIGN_PTR((char*)remote_ptr + size - 1, attach_align, uintptr_t) +
        1;

    using offset_type = decltype(xpmem_addr::offset);
    xpmem_addr addr{.apid = apid, .offset = (offset_type)base};
    auto* ctx = xpmem_attach(addr, bound - base, NULL);
    CmiEnforceMsg(ctx != (void*)-1, "xpmem_attach failed!");

    return (void*)((uintptr_t)ctx +
                   (ptrdiff_t)((uintptr_t)remote_ptr - (uintptr_t)base));
  }
}

static void handleInitialize_(void* msg) {
  auto* imsg = (init_msg_*)msg;
  auto* meta =
      static_cast<ipcManagerXpmem_*>((CsvAccess(managers_))[(imsg->key - 1)].get());
  // extract the segment id and shared region
  // from the msg (registering it in our metadata)
  meta->put_segment(imsg->from, imsg->segid);
  auto* peer = (ipc_shared_*)translateAddr_(meta, imsg->from, imsg->shared,
                                            imsg->span);
  meta->shared[imsg->from].store(peer, std::memory_order_release);
  // then free the message
  CmiFree(imsg);
  // count the segments attached so far (the vector is pre-sized, so
  // occupancy -- not size -- tracks received peer messages)
  int nAttached = 0;
  for (auto& seg : meta->shared) {
    if (seg.load(std::memory_order_relaxed) != nullptr) nAttached++;
  }
  // if we received messages from all our peers:
  if (meta->nPeers == nAttached) {
    if (CmiMyPe() == 0) {
      printIpcStartupMessage_("xpmem");
    }
    finishSetup_(meta);
    awakenSleepers_();
  }
}

static void ipcInitXpmem_(char** argv) {
  CpvInitialize(int, handle_init);
  CpvAccess(handle_init) = CmiRegisterHandler(handleInitialize_);
}

// Runs on rank 0 only, with every other rank of this process waiting on the
// node barrier in CmiMakeIpcManager.
static CmiIpcManager* ipcMakeManagerXpmem_(std::size_t key) {
  auto* meta = new ipcManagerXpmem_(key);
  int* pes;
  int nPes;
  CmiGetPesOnPhysicalNode(CmiPhysicalNodeID(CmiMyPe()), &pes, &nPes);
  meta->nPeers = nPes / CmiMyNodeSize();
  return meta;
}

// Publishes this process's segment id to its peers on this host. Split from
// the constructor so the manager is in the registry before any reply can be
// handled: handleInitialize_ finds it there by key.
static void ipcBootstrapXpmem_(CmiIpcManager* base) {
  auto* meta = static_cast<ipcManagerXpmem_*>(base);
  int* pes;
  int nPes;
  auto thisPe = CmiMyPe();
  CmiGetPesOnPhysicalNode(CmiPhysicalNodeID(thisPe), &pes, &nPes);
  auto nSize = CmiMyNodeSize();
  auto nProcs = meta->nPeers;

  if (nProcs <= 1) {
    // sole process on this host: nothing to attach, so the pool is as ready
    // as it will ever be
    if (CmiMyPe() == 0) printIpcStartupMessage_("xpmem");
    finishSetup_(meta);
    awakenSleepers_();
    return;
  }

  auto* imsg = (init_msg_*)CmiAlloc(sizeof(init_msg_));
  CmiSetHandler(imsg, CpvAccess(handle_init));
  imsg->key = meta->key;
  imsg->from = meta->mine;
  imsg->segid = meta->get_segment(meta->mine);
  imsg->shared = meta->shared[meta->mine].load(std::memory_order_relaxed);
  imsg->span = meta->span;
  // send messages to all the pes on this node
  for (auto i = 0; i < nProcs; i++) {
    auto& pe = pes[i * nSize];
    auto last = i == (nProcs - 1);
    if (pe == thisPe) {
      if (last) {
        CmiFree(imsg);
      }
      continue;
    } else if (last) {
      // free'ing with the last send
      CmiSyncSendAndFree(pe, sizeof(init_msg_), (char*)imsg);
    } else {
      // then sending (without free) otherwise
      CmiSyncSend(pe, sizeof(init_msg_), (char*)imsg);
    }
  }
}
