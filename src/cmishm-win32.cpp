// Windows backend for the IPC pool.
//
// Included by cmishmem.cpp in place of cmishm.cpp (not compiled on its own).
// The pool exists to move messages between processes that share a host, and
// reconverse on Windows has no communication backend, so a job is always a
// single process: the pool never has a peer. This backend therefore does what
// the POSIX one does for a process that is alone on its host -- it gives the
// pool this process's own segment, marks it ready, and stops there -- using
// ordinary memory rather than a named shared mapping.

struct ipcManagerLocal_ : public CmiIpcManager {
  ipc_shared_* segment = nullptr;

  ipcManagerLocal_(std::size_t key) : CmiIpcManager(key) {}

  ~ipcManagerLocal_() override {
    if (segment != nullptr) {
      segment->~ipc_shared_();
      ::operator delete(segment);
    }
  }
};

// no messages to exchange, so no handlers to register
static void ipcInitShm_(char** argv) {}

// Runs on rank 0 only, with every other rank of this process waiting on the
// node barrier in CmiMakeIpcManager.
static CmiIpcManager* ipcMakeManagerShm_(std::size_t key) {
  return new ipcManagerLocal_(key);
}

static void ipcBootstrapShm_(CmiIpcManager* base) {
  auto* meta = static_cast<ipcManagerLocal_*>(base);
  CmiEnforceMsg(CmiNumNodes() == 1,
                "the shared-memory IPC pool needs a multi-process job, which "
                "reconverse does not support on Windows");
  meta->segment = makeIpcShared_();
  meta->shared[meta->mine].store(meta->segment, std::memory_order_release);
  finishSetup_(meta);
  awakenSleepers_();
}
