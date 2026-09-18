// POSIX shared memory backend for the IPC pool.
//
// Included by cmishmem.cpp (not compiled on its own) so that the pool's
// block allocator, the sleeper list and the manager registry are shared with
// the other backends in one translation unit.
//
// Each process creates one shm segment named after the pid of its host's
// leader process and its own logical node number, and maps the segment of
// every other process on the host. The leader's pid is the only thing that
// cannot be derived locally, so it is broadcast over ordinary Converse
// messages before any segment is opened.
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>

CpvStaticDeclare(int, num_cbs_recvd);
CpvStaticDeclare(int, num_cbs_exptd);
CpvStaticDeclare(int, handle_callback);
CpvStaticDeclare(int, handle_node_pid);
CsvStaticDeclare(pid_t, node_pid);

struct ipcManagerShm_;
static int sendPid_(ipcManagerShm_*);
static void openAllShared_(ipcManagerShm_*);

struct pid_message_ {
  char core[CmiMsgHeaderSizeBytes];
  std::size_t key;
  pid_t pid;
};

#define CMI_SHARED_FMT "cmi_pid%lu_node%d_shared_"

// opens a shared memory segment for a given physical rank
static std::pair<int, ipc_shared_*> openShared_(int node) {
  // determine the size of the shared segment
  // (adding the size of the queues and what nots)
  auto size = CpvAccess(kSegmentSize) + sizeof(ipc_shared_);
  // generate a name for this pe
  auto slen = snprintf(NULL, 0, CMI_SHARED_FMT, (std::size_t)CsvAccess(node_pid), node);
  auto name = new char[slen + 1];
  snprintf(name, slen + 1, CMI_SHARED_FMT, (std::size_t)CsvAccess(node_pid), node);
  DEBUGF(("%d> opening share %s\n", CmiMyPe(), name));
  // try opening the share exclusively
  auto fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
  // if we succeed, we're the first accessor, so:
  if (fd >= 0) {
    // truncate it to the correct size
    auto status = ftruncate(fd, size);
    CmiEnforceMsg(status >= 0, "could not size shm segment %s to %zu bytes: "
                  "%s (is /dev/shm large enough for %d processes?)",
                  name, size, strerror(errno), (int)CmiNumNodes());
  } else {
    const int createErrno = errno;
    // otherwise just open it -- but the segment becomes visible to
    // shm_open the instant its creator's O_CREAT succeeds, before that
    // creator has called ftruncate. Every rank races to open every
    // segment name (including its own), so we may win this open before
    // the true creator has resized the file; mmap-ing and touching it
    // while it's still 0 bytes is a SIGBUS. Poll fstat until the size
    // lands.
    fd = shm_open(name, O_RDWR, 0666);
    CmiEnforceMsg(fd >= 0, "could not open shm segment %s: %s (creating it "
                  "failed with: %s)", name, strerror(errno),
                  strerror(createErrno));
    struct stat st;
    const int kMaxAttempts = 10000;  // ~1s at 100us/attempt
    for (auto attempt = 0;; attempt++) {
      // NOTE: this has to stay outside CmiAssert -- an optimized build drops
      // the whole expression, so the call never happens and the loop spins on
      // an uninitialized st until it times out.
      CmiEnforceMsg(fstat(fd, &st) == 0, "could not stat shm segment %s: %s",
                    name, strerror(errno));
      if ((std::size_t)st.st_size >= size) break;
      CmiEnforceMsg(attempt < kMaxAttempts,
                    "timed out waiting for shm segment %s to be sized: it is "
                    "%zu bytes, %zu were expected",
                    name, (std::size_t)st.st_size, size);
      usleep(100);
    }
  }
  // map the segment to an address:
  auto* res = (ipc_shared_*)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  CmiEnforceMsg(res != MAP_FAILED, "could not map shm segment %s (%zu bytes): "
                "%s", name, size, strerror(errno));
  // then delete the name
  delete[] name;
  // return the file descriptor/shared
  return std::make_pair(fd, res);
}

struct ipcManagerShm_ : public CmiIpcManager {
  std::map<int, int> fds;
  // cached values to avoid touching Cpv/Csv at static teardown
  std::size_t mapped_size;
  std::size_t node_pid_val;

  ipcManagerShm_(std::size_t key) : CmiIpcManager(key) {
    // cache map size while CPV/CSV systems are valid; node_pid_val is
    // captured in openAllShared_ once the node pid is actually known
    this->mapped_size = CpvAccess(kSegmentSize) + sizeof(ipc_shared_);
    CsvInitialize(pid_t, node_pid);
    this->node_pid_val = 0;
  }

  virtual ~ipcManagerShm_() {
    // use cached mapped size (set at construction) to avoid touching CPV/CSV
    // during static teardown
    std::size_t map_size = this->mapped_size;

    // for each rank/descriptor pair
    for (auto& pair : this->fds) {
      auto proc = pair.first;
      auto fd = pair.second;
      // only unmap if we have a valid pointer and a non-zero mapping size
      auto* seg = (proc >= 0 && proc < (int)this->shared.size())
                      ? this->shared[proc].load(std::memory_order_relaxed)
                      : nullptr;
      if (seg != nullptr && map_size > 0) {
        munmap(seg, map_size);
      }
      // close the file if valid
      if (fd >= 0) close(fd);
      // unlinking the shm segment for our pe (use cached node pid)
      if (proc == this->mine) {
        auto slen = snprintf(NULL, 0, CMI_SHARED_FMT, this->node_pid_val, proc);
        auto name = new char[slen + 1];
        snprintf(name, slen + 1, CMI_SHARED_FMT, this->node_pid_val, proc);
        shm_unlink(name);
        delete[] name;
      }
    }
  }
};

static void openAllShared_(ipcManagerShm_* meta) {
  int* pes;
  int nPes;
  int thisNode = CmiPhysicalNodeID(CmiMyPe());
  CmiGetPesOnPhysicalNode(thisNode, &pes, &nPes);
  int nSize = CmiMyNodeSize();
  int nProcs = nPes / nSize;
  // the node pid is known by now (set by sendPid_/nodePidHandler_);
  // cache it for the destructor's shm_unlink
  meta->node_pid_val = (std::size_t)CsvAccess(node_pid);
  // for each rank in this physical node:
  for (auto rank = 0; rank < nProcs; rank++) {
    // open its shared segment
    auto pe = pes[rank * nSize];
    auto proc = CmiNodeOf(pe);
    auto res = openShared_(proc);
    // initializing it if it's ours
    if (proc == meta->mine) initIpcShared_(res.second);
    // store the retrieved data
    meta->fds[proc] = res.first;
    // release-store: publishes the mapped (and, for ours, initialized)
    // segment to PE threads polling metadataReady_/CmiAllocIpcBlock
    meta->shared[proc].store(res.second, std::memory_order_release);
  }
  DEBUGF(("%d> finished opening all shared\n", meta->mine));
  finishSetup_(meta);
}

// returns number of processes in node
static int procBroadcastAndFree_(char* msg, std::size_t size) {
  int* pes;
  int nPes;
  int thisPe = CmiMyPe();
  int thisNode = CmiPhysicalNodeID(thisPe);
  CmiGetPesOnPhysicalNode(thisNode, &pes, &nPes);
  int nSize = CmiMyNodeSize();
  int nProcs = nPes / nSize;
  CmiAssert(thisPe == pes[0]);

  CpvAccess(num_cbs_exptd) = nProcs - 1;
  for (auto rank = 1; rank < nProcs; rank++) {
    auto& pe = pes[rank * nSize];
    if (rank == (nProcs - 1)) {
      CmiSyncSendAndFree(pe, size, msg);
    } else {
      CmiSyncSend(pe, size, msg);
    }
  }

  // free if we didn't send anything
  if (nProcs == 1) {
    CmiFree(msg);
  }

  return nProcs;
}

static int sendPid_(ipcManagerShm_* manager) {
  CsvInitialize(pid_t, node_pid);
  CsvAccess(node_pid) = getpid();

  auto* pmsg = (pid_message_*)CmiAlloc(sizeof(pid_message_));
  CmiSetHandler(pmsg, CpvAccess(handle_node_pid));
  pmsg->key = manager->key;
  pmsg->pid = CsvAccess(node_pid);

  return procBroadcastAndFree_((char*)pmsg, sizeof(pid_message_));
}

static void callbackHandler_(void* msg) {
  int mine = CmiMyPe();
  int node = CmiPhysicalNodeID(mine);
  int first = CmiGetFirstPeOnPhysicalNode(node);
  auto* pmsg = (pid_message_*)msg;
  int key = pmsg->key;

  if (mine == first) {
    // if we're still expecting messages:
    if (++(CpvAccess(num_cbs_recvd)) < CpvAccess(num_cbs_exptd)) {
      // free this one
      CmiFree(msg);
      // and move along
      return;
    } else {
      // otherwise -- tell everyone we're ready!
      if (CmiMyPe() == 0) printIpcStartupMessage_("pxshm");
      procBroadcastAndFree_((char*)msg, sizeof(pid_message_));
    }
  } else {
    CmiFree(msg);
  }

  auto& meta = (CsvAccess(managers_))[(key - 1)];
  openAllShared_(static_cast<ipcManagerShm_*>(meta.get()));
  awakenSleepers_();
}

static void nodePidHandler_(void* msg) {
  auto* pmsg = (pid_message_*)msg;
  CsvInitialize(pid_t, node_pid);
  CsvAccess(node_pid) = pmsg->pid;

  int node = CmiPhysicalNodeID(CmiMyPe());
  int root = CmiGetFirstPeOnPhysicalNode(node);
  CmiSetHandler(msg, CpvAccess(handle_callback));
  CmiSyncSendAndFree(root, sizeof(pid_message_), (char*)msg);
}

static void ipcInitShm_(char** argv) {
  CpvInitialize(int, num_cbs_recvd);
  CpvInitialize(int, num_cbs_exptd);
  CpvInitialize(int, handle_callback);
  CpvAccess(handle_callback) = CmiRegisterHandler(callbackHandler_);
  CpvInitialize(int, handle_node_pid);
  CpvAccess(handle_node_pid) = CmiRegisterHandler(nodePidHandler_);
}

// Runs on rank 0 only, with every other rank of this process waiting on the
// node barrier in CmiMakeIpcManager.
static CmiIpcManager* ipcMakeManagerShm_(std::size_t key) {
  CpvAccess(num_cbs_recvd) = CpvAccess(num_cbs_exptd) = 0;
  return new ipcManagerShm_(key);
}

// Starts the pid exchange. Split from the constructor so the manager is in
// the registry before any reply can be handled: callbackHandler_ finds it
// there by key.
static void ipcBootstrapShm_(CmiIpcManager* base) {
  auto* meta = static_cast<ipcManagerShm_*>(base);
  auto firstPe = CmiNodeFirst(CmiMyNode());
  if (CmiPhysicalRank(firstPe) != 0) return;  // not this host's leader process
  if (sendPid_(meta) == 1) {
    // sole process on this host: nobody to hear from, so finish here
    openAllShared_(meta);
    awakenSleepers_();
  }
}
