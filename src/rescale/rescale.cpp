// No-restart shrink/expand: the Converse half.
//
// A rescale changes the set of processes in a running job without restarting
// any of them. The sequence, driven from ConverseCleanup below, is:
//
//   1. Node 0 asks the coordinator to commit a new membership: which of the
//      current members are leaving, and how many waiting newcomers to admit.
//   2. The coordinator replies with the committed view and pushes DIE to the
//      departing members and INTEGRATE to the admitted newcomers.
//   3. Every surviving process learns the same view and hands it to the comm
//      backend, which closes connections to departing peers, keeps the ones
//      that survived, opens connections to the arrivals, and renumbers itself.
//   4. Everyone meets at a barrier over the new membership.
//   5. Survivors longjmp back into charm_main, which re-enters ConverseInit on
//      the survivor path; departing processes exit.
//
// The transport half of step 3 is entirely the backend's business. Everything
// here is transport-independent.

#include "converse_internal.h"

#if CMK_SHRINK_EXPAND

#include <setjmp.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "comm_backend/comm_backend.h"
#include "rescale/coord_client.h"

// Defined in Charm++ (ck-core/init.C), which owns the landing point. The same
// contract the UCX and MPI machine layers use.
extern jmp_buf _shrinkexpand_jmpbuf;

// Defined in convcore.cpp.
extern bool _shrinkexpand_restarting;
extern int _rescaleGeneration;
void CmiSetRescaleRestartState(int myNode, int numNodes);

// Timestamps Charm++ prints as the rescale breakdown. It owns the storage; the
// machine layers stamp them, and so does this. All are PE-0 only, and a wall
// clock rather than CmiWallTimer, whose epoch the rescale spans.
extern double rescale_t_cleanup_enter;
extern double rescale_t_commit_done;
extern double rescale_t_ep_reinit_done;
extern double rescale_t_barrier_done;
extern double rescale_t_longjmp;
extern double rescale_wall_now();

// Defined in convcore.cpp; the pre-teardown flush below uses them.
int CmiRescalePumpQueuesOnce(void);
long CmiRescaleAmSent(void);
long CmiRescaleAmRecv(void);
void CmiRescaleResetAmCounters(void);

namespace {

// Coordinator connection, established at bootstrap and held for the life of
// the process. -1 when the job was not launched with a coordinator, in which
// case no rescale is possible.
int g_coordFd = -1;
uint32_t g_coordEpoch = 0;

// The pending membership change, filled in by Charm++ before it triggers the
// exit path. `g_availVector` has one byte per current node, nonzero meaning
// the node stays. Only node 0 uses it; the others learn the outcome from the
// coordinator.
bool g_rescalePending = false;

// Set by registerNewcomerWarmup; run on a joining process while it waits to be
// admitted. Null when the application layer has nothing to warm up.
void (*g_newcomerWarmup)(void) = nullptr;
std::vector<char> g_availVector;
int g_targetNumNodes = 0;

comm_backend::ClusterView toBackendView(const coord::ClusterView &v) {
  comm_backend::ClusterView out;
  out.epoch = v.epoch;
  out.nodeId = (int)v.nodeId;
  out.members.reserve(v.members.size());
  for (const auto &m : v.members) {
    comm_backend::Member bm;
    bm.nodeId = (int)m.nodeId;
    bm.addr.assign(m.ucxAddr.begin(), m.ucxAddr.end());
    out.members.push_back(bm);
  }
  return out;
}

// The coordinator replies with a delta against the membership the caller
// already holds, so both sides have to start from the same list. The backend
// is the one that keeps it up to date.
std::vector<coord::Member> currentMembers() {
  const std::vector<comm_backend::Member> &cur = comm_backend::getMembers();
  std::vector<coord::Member> out;
  out.reserve(cur.size());
  for (const auto &m : cur) {
    coord::Member cm;
    cm.nodeId = (uint32_t)m.nodeId;
    cm.ucxAddr.assign(m.addr.begin(), m.addr.end());
    out.push_back(cm);
  }
  return out;
}


// ---------------------------------------------------------------------------
// Tree broadcast of the committed delta.
//
// Node 0 negotiates the membership change and then hands the result to the
// other survivors itself, over the endpoints that are still up, instead of
// having the coordinator send it to each of them in turn. The coordinator's
// version costs one send per survivor from a single process, and it sits on
// node 0's critical path because the reply to node 0 is written last; this
// costs a logarithmic number of hops on the job's own transport. It is the
// same shape the UCX machine layer uses.
//
// The payload is the delta alone. Every survivor already holds the old
// membership, so it can rebuild the new list, and its own new node id is just
// its rank among the survivors in old-id order. Nothing per-receiver travels,
// which is what lets one buffer be forwarded down the tree unchanged.

struct FanoutInbox {
  std::vector<uint8_t> payload;
  bool received = false;
};
FanoutInbox g_fanoutInbox;
comm_backend::AmHandler g_fanoutAm = -1;
comm_backend::AmHandler g_wireupAm = -1;
char g_wireupByte = 0;

// The buffer handed to issueAm has to outlive the call: the send is
// asynchronous and the payload is forwarded on to this node's children.
std::vector<uint8_t> g_fanoutOutbox;

void fanoutRecv(comm_backend::Status status) {
  // The backend owns the delivered buffer, so take a copy before returning.
  const uint8_t *b = static_cast<const uint8_t *>(status.local_buf);
  g_fanoutInbox.payload.assign(b, b + status.size);
  g_fanoutInbox.received = true;
}

void fanoutSendDone(comm_backend::Status) {}

// Receives the one-byte probes a joining process sends to force its
// connections open. There is nothing to do with them: the value is the
// handshake they provoke, not the payload.
void wireupRecv(comm_backend::Status) {}

// Survivors keep their relative order, so the survivor at index i in old-id
// order is exactly the process that apply_member_delta numbers i.
std::vector<uint32_t> survivorOldIds(const std::vector<coord::Member> &oldMembers,
                                     const std::vector<uint32_t> &killedOldIds) {
  std::vector<uint8_t> killed;
  for (uint32_t k : killedOldIds) {
    if (k >= killed.size()) killed.resize(k + 1, 0);
    killed[k] = 1;
  }
  std::vector<uint32_t> out;
  for (const auto &m : oldMembers)
    if (m.nodeId >= killed.size() || !killed[m.nodeId]) out.push_back(m.nodeId);
  return out;
}

// Binary tree over the survivors' NEW ids, addressed by their OLD ids because
// the transport has not been reconfigured yet.
void fanoutForward(const std::vector<uint32_t> &survivors, uint32_t myNewId,
                   const std::vector<uint8_t> &payload) {
  if (payload.empty()) return;
  g_fanoutOutbox = payload;
  for (int k = 1; k <= 2; ++k) {
    size_t child = (size_t)myNewId * 2 + k;
    if (child >= survivors.size()) break;
    comm_backend::issueAm((int)survivors[child], g_fanoutOutbox.data(),
                          g_fanoutOutbox.size(), comm_backend::MR_NULL,
                          fanoutSendDone, g_fanoutAm, nullptr);
  }
}

// Spin on the backend until this node's parent forwards the delta. The
// scheduler is stopped here, so progress() is the only thing moving messages,
// and the backend invokes the handler from inside it. The deadline turns a
// lost message into a diagnosable abort rather than a silent hang.
void fanoutWait(std::vector<uint8_t> *out) {
  const double deadline = rescale_wall_now() + 120.0;
  while (!g_fanoutInbox.received) {
    comm_backend::progress();
    if (rescale_wall_now() > deadline)
      CmiAbort("Shrink/expand: timed out waiting for the committed view from "
               "this node's parent in the broadcast tree");
  }
  *out = std::move(g_fanoutInbox.payload);
  g_fanoutInbox.payload.clear();
  g_fanoutInbox.received = false;
}

} // namespace

// Registered from ConverseInit next to the Converse handler and under the same
// once-guard, so survivors and newcomers agree on the index.
void CmiRegisterRescaleFanoutHandler(void) {
  g_fanoutAm = comm_backend::registerAmHandler(fanoutRecv);
  g_wireupAm = comm_backend::registerAmHandler(wireupRecv);
}

void registerNewcomerWarmup(void (*fn)(void)) { g_newcomerWarmup = fn; }

int CmiRescaleCoordFd(void) { return g_coordFd; }

void CmiSetRescaleCoordFd(int fd, unsigned int epoch) {
  g_coordFd = fd;
  g_coordEpoch = epoch;
}

void CmiSetRescalePending(int pending) { g_rescalePending = (pending != 0); }

int CmiRescalePending(void) { return g_rescalePending ? 1 : 0; }

void CmiRescaleRequest(const char *availVector, int numOldNodes,
                       int targetNumNodes) {
  g_availVector.assign(availVector, availVector + numOldNodes);
  g_targetNumNodes = targetNumNodes;
}

// Bootstrap against a coordinator instead of the launcher's process manager.
// Returns nonzero if the coordinator supplied this process's identity, in which
// case *myNodeId and *numNodes are filled in and the backend has been wired up
// to the initial membership.
int CmiRescaleCoordBootstrap(const char *coordHost, int coordPort,
                             int launcherNodeId, int launcherNumNodes,
                             int isNewcomer, int *myNodeId, int *numNodes) {
  if (!comm_backend::supportsRescale()) return 0;

  g_coordFd = coord::connect_blocking(coordHost, coordPort);
  if (g_coordFd < 0) {
    CmiPrintf("Charm> could not reach coordinator at %s:%d\n", coordHost,
              coordPort);
    return 0;
  }

  std::vector<unsigned char> myAddr = comm_backend::getMyAddress();
  coord::ClusterView view;

  if (isNewcomer) {
    // Register, then block until a commit admits us. The wait is typically
    // seconds (the running job rescales at its next load balancing step), so
    // it is the natural place to overlap any expensive local warmup.
    if (!coord::register_newcomer(g_coordFd, myAddr.data(),
                                  (uint32_t)myAddr.size(), &view))
      return 0;
    // Registration is in, admission is seconds away, and nothing else is
    // competing for this process. Do the expensive local setup now rather than
    // after the commit, where every process already in the job waits on it.
    // Wire up to the processes already running, before blocking rather than
    // after being admitted.
    //
    // The registration reply carries the current membership, so the peers are
    // known now, and the wait for admission is seconds. Connecting here means
    // the handshakes overlap that idle time. Left until after the commit they
    // land instead inside the first collective of the new membership, where
    // every process already in the job is waiting at the barrier, and they are
    // the dominant cost of an expansion. This is what the UCX machine layer
    // does with speculative endpoints.
    //
    // The membership may still change before admission, so this is a
    // prediction, not a commitment: the view applied after INTEGRATE is the
    // real one. Connections made here to peers that survive are kept, because
    // reconfigure carries unchanged peers across untouched.
    // Needed before any send; otherwise set up later in converseRunPe.
    // Shrink/expand runs one PE per process, so this is that PE.
    comm_backend::initThread(0, 1);
    // Off by default. This used to take the job down whenever a newcomer
    // registered, because handler registration ran after this bootstrap and
    // the sends below went out with the initializer value -1 as the handler
    // index; peers dispatched that out of bounds. Registration now happens
    // before the bootstrap, so the index is valid, but the path has not been
    // measured end to end and the payoff is small: wireup costs 1.0-1.5 ms for
    // three peers, against a GPU expansion of a few ms in total. Note also
    // that UCX's equivalent is not an application message at all -- ucp_ep_create
    // plus ucp_worker_flush is a transport-level handshake that delivers
    // nothing to the peer, and LCI has no such primitive.
    const char* wireupEnv = getenv("CHARM_NEWCOMER_WIREUP");
    const bool wireupEnabled = wireupEnv && strcmp(wireupEnv, "0") != 0;
    if (wireupEnabled && !view.members.empty()) {
      double t0 = rescale_wall_now();
      // A rank is needed to reconfigure, and this process does not have its
      // real one yet; anything outside the current membership will do, since
      // nothing is addressed to it before admission.
      coord::ClusterView speculative = view;
      coord::Member self;
      self.nodeId = (uint32_t)view.members.size();
      self.ucxAddr.assign(myAddr.begin(), myAddr.end());
      speculative.members.push_back(self);
      speculative.nodeId = self.nodeId;
      comm_backend::reconfigure(toBackendView(speculative));
      // Sending is what opens a connection; the address vector entry alone
      // does not. One byte to each peer is enough to provoke the handshake.
      for (size_t i = 0; i + 1 < speculative.members.size(); ++i) {
        comm_backend::issueAm((int)i, &g_wireupByte, sizeof(g_wireupByte),
                              comm_backend::MR_NULL, fanoutSendDone,
                              g_wireupAm, nullptr);
      }
      comm_backend::drain();
      CmiPrintf("Charm> newcomer wired up to %zu peers in %.6fs while waiting "
                "to be admitted\n",
                speculative.members.size() - 1, rescale_wall_now() - t0);
    }

    if (g_newcomerWarmup) {
      double t0 = rescale_wall_now();
      g_newcomerWarmup();
      CmiPrintf("Charm> newcomer warmup took %.6fs while waiting to be "
                "admitted\n", rescale_wall_now() - t0);
    }
    if (!coord::await_integrate(g_coordFd, &view)) return 0;
  } else {
    // The coordinator was told how many initial ranks to expect when it was
    // started, so it holds the reply until all of them have checked in.
    (void)launcherNumNodes;
    if (!coord::register_initial(g_coordFd, (uint32_t)launcherNodeId,
                                 myAddr.data(), (uint32_t)myAddr.size(), &view))
      return 0;
  }

  g_coordEpoch = view.epoch;
  _rescaleGeneration = (int)view.epoch;
  comm_backend::reconfigure(toBackendView(view));

  if (isNewcomer) {
    // Meet the processes already in the job. The backend's own barrier is
    // usable here, and is the whole point of resetting the collective
    // sequence during reconfigure: this process and the ones already running
    // are back in step, so they can rendezvous over the network rather than
    // through the coordinator. With the wireup above already done, this is a
    // round trip over connections that exist rather than the place they get
    // built.
    comm_backend::barrier();
  }

  *myNodeId = (int)view.nodeId;
  *numNodes = (int)view.members.size();
  return 1;
}

// Called by Charm++ from its exit handler. On a normal exit this returns and
// the caller proceeds to ConverseExit. On a rescale it never returns: the
// process either longjmps back into charm_main or exits.
// Set by Charm++ at init: a departing node calls this before the exit flush
// so higher-layer state that only exists as *held messages* (an interior
// reduction node's partials) is forwarded to survivors while the transport
// still includes everyone. Null when the layer above has nothing to flush.
extern "C" {
void (*CmiRescaleDoomedFlushFn)(void) = nullptr;

// Old-world PE -> new-world PE across the most recent committed rescale,
// computed from the availability vector that drove it (which survives the
// longjmp until the next request overwrites it). Identity when no rescale
// has happened; -1 for a PE that left. Valid for exactly one generation --
// callers gate on their own generation stamps.
int CmiRescaleOldPeToNew(int oldPe) {
  if (oldPe < 0 || oldPe >= (int)g_availVector.size()) return oldPe;
  if (!g_availVector[oldPe]) return -1;
  int rank = 0;
  for (int i = 0; i < oldPe; i++)
    if (g_availVector[i]) rank++;
  return rank;
}
}

void ConverseCleanup(void) {
  if (!CmiRescalePending()) return;

  // What follows -- the drain, the cut, and the restore on the way back -- is
  // a bootstrap sequence, not application concurrency. Close the reordering
  // window; the layer above opens it again once the restore is complete.
  CmiRandomizedQueueSuspend();

  if (!comm_backend::supportsRescale()) {
    CmiAbort("Shrink/expand was requested but the active communication "
             "backend cannot reconfigure a running job. Build with a backend "
             "that implements the rescale hooks in comm_backend.h.");
  }
  if (g_coordFd < 0) {
    CmiAbort("Shrink/expand was requested but this job was not launched with "
             "a coordinator (+coordinator host:port).");
  }

  const int oldNumNodes = CmiNumNodes();
  const int myNode = CmiMyNode();
  if (myNode == 0) rescale_t_cleanup_enter = rescale_wall_now();
  const std::vector<coord::Member> oldMembers = currentMembers();

  // Every node holds the availability vector (Charm++ broadcasts it before it
  // triggers this path), so each one knows locally whether it is leaving. That
  // is what lets the departing nodes stay on the coordinator socket for DIE
  // while the survivors take the delta from the tree: nobody forwards to a
  // node that is about to exit.
  bool departing = (myNode < (int)g_availVector.size() && !g_availVector[myNode]);

  // Give the layer above one chance to forward held state (see the pointer's
  // comment) before anything is quiesced; the sends it makes are counted and
  // drained by the flush below.
  if (departing && CmiRescaleDoomedFlushFn) CmiRescaleDoomedFlushFn();

  // Quiesce the old membership before anyone's peer table is touched.
  //
  // drain() only fences operations this process POSTED: for an eager active
  // message, completion means injection, not arrival, so after a plain
  // drain+barrier a message can still be in the network -- and a barrier-less
  // rescale reaches here with application traffic in full flight (observed:
  // ghosts lost at the cut, application wedged after restore). Flush instead
  // until the cluster-wide send and arrival counters agree. This is
  // quiescence detection with "processed" counted at arrival-into-queue
  // rather than at delivery: gating delivery cannot starve it, and the
  // arrived-but-undelivered messages it leaves behind sit in survivor queues
  // that live across the longjmp.
  //
  // A departing node additionally delivers everything already queued on it:
  // those are strays for elements evacuated off it, and delivering them here
  // lets the location layer forward each to the element's new host while the
  // transport still includes everyone. Left queued, they would exit with the
  // process. The equality must hold in two consecutive rounds with nothing
  // pumped anywhere, guarding the window between a node sampling its counters
  // and the reduction reading them.
  {
    long prev[2] = {-1, -1};
    const int maxRounds = 4000;
    int round = 0;
    for (;; ++round) {
      if (round >= maxRounds) {
        CmiPrintf("Charm> Warning: rescale flush did not converge after %d "
                  "rounds (sent=%ld arrived=%ld); proceeding anyway.\n",
                  round, prev[0], prev[1]);
        break;
      }
      long pumped = 0;
      if (departing) pumped = (long)CmiRescalePumpQueuesOnce();
      comm_backend::drain();
      comm_backend::barrier();
      long v[3] = {CmiRescaleAmSent(), CmiRescaleAmRecv(), pumped};
      comm_backend::allreduceSumLong(v, 3);
      if (v[0] == v[1] && v[2] == 0 && v[0] == prev[0] && v[1] == prev[1])
        break;
      prev[0] = v[0];
      prev[1] = v[1];
    }
    if (myNode == 0 && round > 1)
      CmiPrintf("Charm> Rescale flush: %d rounds to quiesce the old world.\n",
                round + 1);
    // Counters restart each epoch: a departing process takes its share of
    // both sums with it, and nothing is in flight right now, so zero is the
    // one value every survivor and every future newcomer can agree on.
    CmiRescaleResetAmCounters();
  }

  coord::ClusterView view;

  if (departing) {
    coord::ClusterView ignored;
    bool gotDie = false;
    coord::await_reconfig_or_die(g_coordFd, oldMembers, &ignored, &gotDie);
    // Nothing to tear down: the survivors have already dropped their
    // connections to this process, and the OS reclaims the rest. Calling the
    // collective teardown here would hang, since no one else is in it.
    ::close(g_coordFd);
    ::_exit(0);
  }

  // The committed delta, in the shape the coordinator would have pushed:
  // epoch, the old ids that left, and the members that joined.
  std::vector<uint8_t> delta;
  std::vector<uint32_t> killedOldIds;
  std::vector<coord::Member> added;

  if (myNode == 0) {
    // Node 0 drives the commit. The avail vector is in the current (old) node
    // numbering; a zero entry means that node is leaving.
    if ((int)g_availVector.size() < oldNumNodes) {
      CmiAbort("Shrink/expand: node 0 reached the exit path without an "
               "availability vector for the current membership");
    }
    std::vector<uint32_t> kills;
    int survivors = 0;
    for (int i = 0; i < oldNumNodes; ++i) {
      if (g_availVector[i])
        survivors++;
      else
        kills.push_back((uint32_t)i);
    }

    // Admit enough waiting newcomers to reach the requested size, capped by
    // how many have actually registered.
    uint32_t take = (g_targetNumNodes > survivors)
                        ? (uint32_t)(g_targetNumNodes - survivors)
                        : 0u;
    uint32_t pending = 0;
    if (!coord::query_pending(g_coordFd, &pending))
      CmiAbort("Shrink/expand: coordinator QUERY_PENDING failed");
    if (take > pending) {
      CmiPrintf("Charm> coordinator: requested %u newcomers, only %u "
                "available\n",
                take, pending);
      take = pending;
    }

    if (!coord::commit(g_coordFd, g_coordEpoch, kills, take, oldMembers, &view))
      CmiAbort("Shrink/expand: coordinator COMMIT failed");

    CmiPrintf("Charm> coordinator COMMIT: epoch %u->%u, %zu leaving, %u "
              "joining, %zu members\n",
              g_coordEpoch, view.epoch, kills.size(), take,
              view.members.size());

    // apply_member_delta lays the new list out as survivors first, in old-id
    // order, then the arrivals, so the tail past the survivor count is exactly
    // what was added.
    killedOldIds = kills;
    // Slice against the list the coordinator actually built the view from,
    // rather than a separately maintained count of nodes.
    const size_t nSurvivors = oldMembers.size() - kills.size();
    if (view.members.size() < nSurvivors)
      CmiAbort("Shrink/expand: committed view holds %zu members, fewer than "
               "the %zu survivors it was built from",
               view.members.size(), nSurvivors);
    added.assign(view.members.begin() + nSurvivors, view.members.end());
    coord::put_u32(delta, view.epoch);
    coord::put_u32_vec(delta, killedOldIds);
    coord::put_members(delta, added);
  } else {
    // Surviving nodes take the delta from their parent in the tree rather
    // than from the coordinator, so the commit stays a single round trip on
    // node 0 whose cost does not grow with the number of survivors.
    fanoutWait(&delta);
    const uint8_t *q = delta.data();
    const uint8_t *end = q + delta.size();
    view.epoch = coord::get_u32(q, end);
    killedOldIds = coord::get_u32_vec(q, end);
    added = coord::get_members(q, end);
    view.members = coord::apply_member_delta(oldMembers, killedOldIds, added);
  }

  // Forward before touching this node's own endpoints: the tree is addressed
  // in the old numbering, which reconfigure is about to invalidate. Node 0
  // already knows its id is 0; everyone else is its rank among the survivors.
  {
    const std::vector<uint32_t> survivors =
        survivorOldIds(oldMembers, killedOldIds);
    uint32_t myNewId = 0;
    for (size_t i = 0; i < survivors.size(); ++i)
      if ((int)survivors[i] == myNode) { myNewId = (uint32_t)i; break; }
    if (myNode != 0) view.nodeId = myNewId;
    fanoutForward(survivors, myNewId, delta);
    // An outstanding send names an address reconfigure is about to remove, so
    // let the forwards complete before the peer table is rebuilt.
    comm_backend::drain();
  }

  if (myNode == 0) rescale_t_commit_done = rescale_wall_now();

  g_coordEpoch = view.epoch;
  // Every rank agrees on the epoch by protocol, which is what makes it usable
  // as a cluster-wide generation number. A per-process counter would diverge
  // between original survivors and newcomer-lineage processes.
  _rescaleGeneration = (int)view.epoch;

  comm_backend::reconfigure(toBackendView(view));
  if (myNode == 0) rescale_t_ep_reinit_done = rescale_wall_now();

  // Meet everyone, old and new, on the reconfigured transport before any of
  // us starts sending. Survivors are about to re-run initialization and
  // newcomers are about to run theirs.
  //
  // This is the backend's own barrier, over the new membership, rather than a
  // round trip per process through the coordinator: the coordinator does no
  // work for it, and its cost grows with the logarithm of the job rather than
  // linearly. The reset of the collective sequence during reconfigure is what
  // lets a process that just joined take part in it.
  comm_backend::barrier();
  if (myNode == 0) rescale_t_barrier_done = rescale_wall_now();

  CmiSetRescalePending(false);
  CmiSetRescaleRestartState((int)view.nodeId, (int)view.members.size());
  _shrinkexpand_restarting = true;
  if (myNode == 0) rescale_t_longjmp = rescale_wall_now();
  longjmp(_shrinkexpand_jmpbuf, 1);
}

#else // !CMK_SHRINK_EXPAND

#include "converse.h"

void ConverseCleanup(void) {}

#endif // CMK_SHRINK_EXPAND
