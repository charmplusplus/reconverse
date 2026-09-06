#include "converse_internal.h"
#include <thread>

int Cmi_bcastHandler;
int Cmi_nodeBcastHandler;
int Cmi_reduceHandler;
int Cmi_nodeReduceHandler;
int Cmi_multicastHandler;
thread_local int CmiGroupHandlerIndex;
thread_local GroupDef *CmiGroupTable;
thread_local int CmiGroupCounter;

static constexpr unsigned int CmiLogMaxReductions = 4u;
static constexpr int CmiMaxReductions = 1u << CmiLogMaxReductions;

void collectiveInit(void) {
  // register special operation handlers
  Cmi_bcastHandler = CmiRegisterHandler(CmiBcastHandler);
  Cmi_nodeBcastHandler = CmiRegisterHandler(CmiNodeBcastHandler);
  CmiGroupHandlerIndex = CmiRegisterHandler(CmiGroupHandler);
  Cmi_reduceHandler = CmiRegisterHandler(CmiReduceHandler);
  Cmi_nodeReduceHandler = CmiRegisterHandler(CmiNodeReduceHandler);
  CmiGroupTable = (GroupDef *)calloc(GROUPTAB_SIZE, sizeof(GroupDef));
  CmiGroupCounter = 0;
  CmiReductionsInit();
}

/************* Broadcasts ***************/

#if SPANTREE

/* The broadcast spanning tree is rooted at the *sender*, not at node 0.
 *
 * Nodes are numbered relative to the root, rel = (node - root) mod CmiNumNodes(),
 * so the sender is always rel 0, and the tree is the ordinary arity-CST_W tree
 * over rel. Every node still appears exactly once, and the sender is able to
 * push the message out itself instead of mailing it to node 0 and waiting for
 * node 0 to relay it.
 *
 * That property is what makes broadcast usable during startup. A relay hop only
 * runs when the relaying PE reaches the scheduler, and a broadcaster frequently
 * does not go back to the scheduler after broadcasting: Charm++'s
 * _sendReadonlies() broadcasts and then calls _initDone(), which blocks in
 * CmiNodeBarrier() until every other PE has received that same broadcast.
 * Routing through a fixed root made the sender its own relay whenever it was
 * the root, so the message sat in its queue and the run deadlocked.
 *
 * Within a node the message is handed to peers directly rather than through a
 * second tree. Peers receive it with the user's handler already restored, so
 * they are leaves and do no forwarding; only the PE that a node's copy first
 * lands on (rank 0, or the sender itself on the root node) relays onward.
 */

// Send to the child nodes of my node in the tree rooted at rootNode. sendFn
// performs the delivery, so this serves both the PE-level broadcast (which
// targets rank 0 of each child node) and the node-level broadcast.
template <typename SendFn>
static void CmiSpanTreeForwardToNodes(int rootNode, SendFn &&sendFn) {
  const int numNodes = CmiNumNodes();
  int rel = CmiMyNode() - rootNode;
  if (rel < 0)
    rel += numNodes;

  for (int i = 0; i < CST_W; i++) {
    const int64_t childRel = (int64_t)rel * CST_W + i + 1;
    if (childRel >= numNodes)
      break;
    int childNode = (int)childRel + rootNode;
    if (childNode >= numNodes)
      childNode -= numNodes;
    sendFn(childNode);
  }
}

// Hand the message to every other PE on my node. They receive it with the
// user's handler, not the broadcast handler, so they do not forward it again.
static void CmiBcastSendToPeers(void *msg, int size) {
  const int nodeSize = CmiMyNodeSize();
  if (nodeSize <= 1)
    return;

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  const int first = CmiNodeFirst(CmiMyNode());
  const int myRank = CmiMyRank();

  const CmiInt2 forwardHandler = header->handlerId;
  header->handlerId = header->swapHandlerId;
  for (int r = 0; r < nodeSize; r++) {
    if (r != myRank)
      CmiSyncSend(first + r, size, msg);
  }
  header->handlerId = forwardHandler;
}

// One relay step of a PE-level broadcast, performed by whichever PE currently
// holds msg. Expects handlerId == Cmi_bcastHandler and swapHandlerId == the
// user's handler.
static void CmiBcastForward(void *msg, int size) {
  CmiSpanTreeForwardToNodes((int)CmiGetBcastRoot(msg), [&](int childNode) {
    CmiSyncSend(CmiNodeFirst(childNode), size, msg);
  });
  CmiBcastSendToPeers(msg, size);
}

// One relay step of a node-level broadcast.
static void CmiNodeBcastForward(void *msg, int size) {
  CmiSpanTreeForwardToNodes((int)CmiGetBcastRoot(msg), [&](int childNode) {
    CmiSyncNodeSend(childNode, size, msg);
  });
}

#endif

/* Broadcast to everyone but the source pe. Source does not free. */
void CmiSyncBroadcast(int size, void *msg) {
  DEBUGF("[%d] CmiSyncBroadcast\n", CmiMyPe());
  int pe = CmiMyPe();

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#if SPANTREE
  DEBUGF("[%d] Spanning tree option\n", CmiMyPe());
  CmiSetBcastRoot(msg, CmiMyNode());
  header->swapHandlerId = header->handlerId;
  header->handlerId = Cmi_bcastHandler;
  CmiBcastForward(msg, size);
  // The caller still owns msg and may keep using it, so hand it back unchanged.
  header->handlerId = header->swapHandlerId;
#else
  for (int i = pe + 1; i < CmiNumPes(); i++)
    CmiSyncSend(i, size, msg);

  for (int i = 0; i < pe; i++)
    CmiSyncSend(i, size, msg);
#endif
}

void CmiSyncBroadcastAndFree(int size, void *msg) {
  CmiSyncBroadcast(size, msg);
  CmiFree(msg);
}

void CmiSyncBroadcastAll(int size, void *msg) {
  DEBUGF("[%d] CmiSyncBroadcastAll\n", CmiMyPe());
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#if SPANTREE
  // Queue our own copy first, with the user's handler still in place, exactly
  // as the non-spanning-tree path does. Delivering it inline instead would run
  // the handler reentrantly inside the broadcast.
  CmiSyncSend(CmiMyPe(), size, msg);
  CmiSyncBroadcast(size, msg);
#else
  for (int i = 0; i < CmiNumPes(); i++)
    CmiSyncSend(i, size, msg);
#endif
}

void CmiSyncBroadcastAllAndFree(int size, void *msg) {
  CmiSyncBroadcastAll(size, msg);
  CmiFree(msg);
}

void CmiWithinNodeBroadcast(int size, void *msg) {
  for (int i = 0; i < CmiMyNodeSize(); i++) {
    int destPe = CmiMyNode() * CmiMyNodeSize() + i;
    CmiSyncSend(destPe, size, msg);
  }
}

void CmiSyncNodeBroadcast(unsigned int size, void *msg) {

  int node = CmiMyNode();

  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#if SPANTREE
  CmiSetBcastRoot(msg, node);
  header->swapHandlerId = header->handlerId;
  header->handlerId = Cmi_nodeBcastHandler;
  CmiNodeBcastForward(msg, size);
  header->handlerId = header->swapHandlerId;
#else

  for (int i = node + 1; i < CmiNumNodes(); i++)
    CmiSyncNodeSend(i, size, msg);

  for (int i = 0; i < node; i++)
    CmiSyncNodeSend(i, size, msg);
#endif
}

void CmiSyncNodeBroadcastAndFree(unsigned int size, void *msg) {
  CmiSyncNodeBroadcast(size, msg);
  CmiFree(msg);
}

void CmiSyncNodeBroadcastAll(unsigned int size, void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->messageSize = size;

#if SPANTREE
  CmiSyncNodeSend(CmiMyNode(), size, msg);
  CmiSyncNodeBroadcast(size, msg);
#else

  for (int i = 0; i < CmiNumNodes(); i++)
    CmiSyncNodeSend(i, size, msg);
#endif
}

void CmiSyncNodeBroadcastAllAndFree(unsigned int size, void *msg) {
  CmiSyncNodeBroadcastAll(size, msg);
  CmiFree(msg);
}

/* Handler for broadcast via the spanning tree. Runs on the first PE of each
 * non-root node; the root node's copy is forwarded inline by the sender. */
void CmiBcastHandler(void *msg) {
#if SPANTREE
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);

  // Forward before delivering: the user's handler takes ownership of msg.
  CmiBcastForward(msg, header->messageSize);

  header->handlerId = header->swapHandlerId;
  CmiCallHandler(header->swapHandlerId, msg);
#endif
}

/* Handler for node broadcast via the spanning tree. */
void CmiNodeBcastHandler(void *msg) {
#if SPANTREE
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);

  CmiNodeBcastForward(msg, header->messageSize);

  header->handlerId = header->swapHandlerId;
  CmiCallHandler(header->swapHandlerId, msg);
#endif
}

void CmiSetBcastRoot(void *msg, CmiBroadcastRoot root) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->collectiveMetaInfo = root;
}

CmiBroadcastRoot CmiGetBcastRoot(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  return header->collectiveMetaInfo;
}

/************* Reductions ***************/

// GENERAL REDUCTION TOOLS

// each pe/thread will call this function
void CmiReductionsInit(void) {
  CpvInitialize(CmiReduction **, _reduction_info);
  CpvInitialize(CmiReductionID, _reduction_counter);

  // allocating memory for reduction info table
  auto redinfo =
      (CmiReduction **)malloc(CmiMaxReductions * sizeof(CmiReduction *));
  for (int i = 0; i < CmiMaxReductions; ++i)
    redinfo[i] = NULL;
  CpvAccess(_reduction_info) = redinfo;

  // each pe will have its own reduction counter, and sets it to 0
  CpvAccess(_reduction_counter) = 0;

  // SETUP NODE-LEVEL REDUCTIONS
  if(CmiMyRank() == 0)
  {
    CsvInitialize(CmiNodeReduction *, _node_reduction_info);
    CsvInitialize(CmiNodeReductionID, _node_reduction_counter);


  auto noderedinfo =
      (CmiNodeReduction *)malloc(CmiMaxReductions * sizeof(CmiNodeReduction));
  for (int i = 0; i < CmiMaxReductions; ++i) {
    CmiNodeReduction &nodered = noderedinfo[i];

    // node reduction must be initialized with a valid lock
    nodered.lock = CmiCreateLock(); // in non-smp this would just be a nullptr
  
    CsvAccess(_node_reduction_info) = noderedinfo;
    CsvAccess(_node_reduction_counter) = 0;
  }

}

}

// extract reduction ID from message
CmiReductionID CmiGetRedID(void *msg) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  return header->collectiveMetaInfo;
}

// set reduction ID for a reduction message
void CmiSetRedID(void *msg, CmiReductionID redID) {
  CmiMessageHeader *header = static_cast<CmiMessageHeader *>(msg);
  header->collectiveMetaInfo = redID;
}

// get next ID with wraparound for creating new reductions
static inline CmiReductionID getNextID(CmiReductionID &ctr) {
  CmiReductionID old = ctr;
  CmiReductionID next = old + 1;
  if (next < old) {
    next = 0;
  }
  ctr = next;
  return old;
}

// TODO: is this needed for node reductions?? old Converse uses locks and
// atomics in SMP for node reductions
// TODO: this overflow is not thread-safe
static inline CmiReductionID getNextID(std::atomic<CmiReductionID> &ctr) {
  CmiReductionID old =
      ctr.fetch_add(1, std::memory_order_relaxed); // Increment atomically
  CmiReductionID next = old + 1;
  if (next < old) {                          // Check for overflow
    ctr.store(0, std::memory_order_relaxed); // Reset to 0 on overflow
  }
  return old;
}


unsigned CmiGetReductionIndex(CmiReductionID id) {
  // treating the id as the index into the reduction table
  // utilized in getCreateReduction and clearReduction to find the reduction
  // struct associated with an ID
  if (id >= CmiMaxReductions) {
    CmiAbort("CmiGetReductionIndex: id >= CmiMaxReductions");
  }
  return id;
}


// PROCESS REDUCTIONS
static void CmiClearReduction(CmiReductionID id) {
  auto &reduction_ref = CpvAccess(_reduction_info)[CmiGetReductionIndex(id)];
  CmiReduction *red = reduction_ref;
  if (red != NULL) {
    free(red->remotebuffer);
    // we assume the user is freeing the actual messages that the buffer is
    // holding or is that something we do?
    free(red);
  }
  reduction_ref = NULL;
}

CmiReductionID CmiGetNextReductionID() {
  return getNextID(CpvAccess(_reduction_counter));
}

void CmiResetGlobalReduceSeqID(void) {
  CpvAccess(_reduction_counter) = 0;
}

static CmiReduction *CmiGetCreateReduction(CmiReductionID id) {
  // should handle the 2 cases:
  // 1. a reduction message arrives from a child for ex), but the parent hasn't
  // gotten the chance to create a reduction struct yet
  // 2. a reduction structure already exists adn teh parent has initiaited its
  // own contribution
  auto &reduction_ref = CpvAccess(_reduction_info)[CmiGetReductionIndex(id)];
  CmiReduction *red = reduction_ref;
  if (reduction_ref == NULL) {
    CmiReduction *newred = (CmiReduction *)malloc(sizeof(CmiReduction));
    newred->ReductionID = id;

    int mype = CmiMyPe();
    newred->numChildren = CmiNumSpanTreeChildren(mype);
    newred->parent = CmiSpanTreeParent(mype);

    newred->messagesReceived = 0;
    newred->localContributed = false;
    newred->localbuffer = NULL;
    newred->localbufferSize = 0;
    newred->remotebuffer =
        (void **)malloc(newred->numChildren * sizeof(void *));

    newred->ops.desthandler = NULL;
    newred->ops.mergefn = NULL;

    red = newred;
    reduction_ref = newred;
  }

  return red;
}

// gets called by every PE pariticapting in the reduction
void CmiReduce(void *msg, int size, CmiReduceMergeFn mergeFn) {
  const CmiReductionID id = CmiGetNextReductionID();
  CmiReduction *red = CmiGetCreateReduction(id);
  CmiInternalReduce(msg, size, mergeFn, red);
}

// assuming the void* msg is dynamically allocated
void CmiInternalReduce(void *msg, int size, CmiReduceMergeFn mergeFn,
                       CmiReduction *red) {
  red->localContributed = true;
  red->localbuffer = msg;
  red->localbufferSize = size;

  red->ops.desthandler = (CmiHandler)CmiGetHandlerFunction(msg);
  red->ops.mergefn = mergeFn;
  CmiSendReduce(red);
}

void CmiSendReduce(CmiReduction *red) {

  // if we haven't received all the messages yet, we can't send the reduction up
  // to the parent
  if (!red->localContributed || red->numChildren != red->messagesReceived) {
    return;
  }

  // anything below this assumes that we are either a leaf node or a nonleaf
  // node but have received all our messages
  void *mergedData = red->localbuffer;
  int msg_size = red->localbufferSize;

  // if we are an nonleaf node, we need to merge the data from our children
  if (red->numChildren > 0) {
    mergedData = (red->ops.mergefn)(&msg_size, red->localbuffer,
                                    red->remotebuffer, red->numChildren);
  }
  void *msg = mergedData;
  // if we have a parent
  if (red->parent != -1) {
    // dont think we need this part anymore because we register this in the
    // beginning
    CmiSetHandler(msg, Cmi_reduceHandler);

    // we need metadata to store the reduction ID itself?
    // because we need to know that a) its a reduction message and b) what
    // reduction ID it is
    CmiSetRedID(msg, red->ReductionID);

    CmiSyncSendAndFree(red->parent, msg_size, msg);
  } else {
    (red->ops.desthandler)(msg);
  }
  CmiClearReduction(red->ReductionID);
}

void CmiReduceHandler(void *msg) {
  CmiReduction *reduction = CmiGetCreateReduction(CmiGetRedID(msg));

  // how are we ensuring the messages arrive in order again?
  reduction->remotebuffer[reduction->messagesReceived] = (char *)msg;
  reduction->messagesReceived++;
  CmiSendReduce(reduction);
}

// NODE REDUCTION
static void CmiClearNodeReduction(CmiReductionID id) {
  auto &reduction_ref =
      CsvAccess(_node_reduction_info)[CmiGetReductionIndex(id)].red;
  CmiReduction *red = reduction_ref;
  if (red != NULL) {
    free(red->remotebuffer);
    // we assume the user is freeing the actual messages that the buffer is
    // holding or is that something we do?
    free(red);
  }
  reduction_ref = NULL;
}

// lock and unlock are used to support SMP
void CmiNodeReduce(void *msg, int size, CmiReduceMergeFn mergeFn) {
  const CmiReductionID id = CmiGetNextNodeReductionID();

  CmiNodeReduction nodeRed =
      CsvAccess(_node_reduction_info)[CmiGetReductionIndex(id)];
  CmiLock(nodeRed.lock);

  CmiReduction *red = CmiGetCreateNodeReduction(id);
  CmiInternalNodeReduce(msg, size, mergeFn, red);

  CmiUnlock(nodeRed.lock);
}

CmiReductionID CmiGetNextNodeReductionID() {
  return getNextID(CsvAccess(_node_reduction_counter));
}

void CmiResetGlobalNodeReduceSeqID(){
  CsvAccess(_node_reduction_counter) = 0;
}

static CmiReduction *CmiGetCreateNodeReduction(CmiReductionID id) {
  // should handle the 2 cases:
  // 1. a reduction message arrives from a child for ex), but the parent hasn't
  // gotten the chance to create a reduction struct yet
  // 2. a reduction structure already exists adn teh parent has initiaited its
  // own contribution
  auto &reduction_ref =
      CsvAccess(_node_reduction_info)[CmiGetReductionIndex(id)].red;
  CmiReduction *red = reduction_ref;
  if (reduction_ref == NULL) {
    CmiReduction *newred = (CmiReduction *)malloc(sizeof(CmiReduction));
    newred->ReductionID = id;

    int mynode = CmiMyNode();
    newred->numChildren = CmiNumNodeSpanTreeChildren(mynode);
    newred->parent = CmiNodeSpanTreeParent(mynode);

    newred->messagesReceived = 0;
    newred->localContributed = false;
    newred->localbuffer = NULL;
    newred->localbufferSize = 0;
    newred->remotebuffer =
        (void **)malloc(newred->numChildren * sizeof(void *));

    newred->ops.desthandler = NULL;
    newred->ops.mergefn = NULL;

    red = newred;
    reduction_ref = newred;
  }

  return red;
}

void CmiInternalNodeReduce(void *msg, int size, CmiReduceMergeFn mergeFn,
                           CmiReduction *red) {
  red->localContributed = true;
  red->localbuffer = msg;
  red->localbufferSize = size;

  red->ops.desthandler = (CmiHandler)CmiGetHandlerFunction(msg);
  red->ops.mergefn = mergeFn;
  CmiSendNodeReduce(red);
}

void CmiSendNodeReduce(CmiReduction *red) {

  // if we haven't received all the messages yet, we can't send the reduction up
  // to the parent
  if (!red->localContributed || red->numChildren != red->messagesReceived) {
    return;
  }

  // anything below this assumes that we are either a leaf node or a nonleaf
  // node but have received all our messages
  void *mergedData = red->localbuffer;
  int msg_size = red->localbufferSize;

  // if we are an nonleaf node, we need to merge the data from our children
  if (red->numChildren > 0) {
    mergedData = (red->ops.mergefn)(&msg_size, red->localbuffer,
                                    red->remotebuffer, red->numChildren);
  }
  void *msg = mergedData;
  // if we have a parent
  if (red->parent != -1) {
    // dont think we need this part anymore because we register this in the
    // beginning
    CmiSetHandler(msg, Cmi_nodeReduceHandler);

    // we need metadata to store the reduction ID itself?
    // because we need to know that a) its a reduction message and b) what
    // reduction ID it is
    CmiSetRedID(msg, red->ReductionID);
    CmiSyncSendAndFree(red->parent, msg_size, msg);
  } else {
    (red->ops.desthandler)(msg);
  }
  CmiClearNodeReduction(red->ReductionID);
}

// lock and unlock used to support SMP
void CmiNodeReduceHandler(void *msg) {

  CmiNodeReduction nodeRed =
      CsvAccess(_node_reduction_info)[CmiGetReductionIndex(CmiGetRedID(msg))];
  CmiLock(nodeRed.lock);

  CmiReduction *reduction = CmiGetCreateNodeReduction(CmiGetRedID(msg));

  // how are we ensuring the messages arrive in order again?
  reduction->remotebuffer[reduction->messagesReceived] = (char *)msg;
  reduction->messagesReceived++;
  CmiSendNodeReduce(reduction);


  CmiUnlock(nodeRed.lock);

}

/************* Groups ***************/
void CmiGroupHandler(void *msg) {
  GroupDef def = (GroupDef)msg;
  GroupDef *table = CmiGroupTable;
  unsigned int hashval, bucket;
  hashval = (def->group.id ^ def->group.pe);
  bucket = hashval % GROUPTAB_SIZE;
  def->next = table[bucket];
  GroupDef newdef =
      (GroupDef)CmiAlloc(sizeof(struct GroupDef_s) + (def->npes * sizeof(int)));
  memcpy(newdef, def, sizeof(struct GroupDef_s) + (def->npes * sizeof(int)));
  table[bucket] = newdef;
}

CmiGroup CmiEstablishGroup(int npes, int *pes) {
  CmiGroup grp;
  GroupDef def;
  int len, i;
  grp.id = CmiGroupCounter++;
  grp.pe = CmiMyPe();
  len = sizeof(struct GroupDef_s) + (npes * sizeof(int));
  def = (GroupDef)CmiAlloc(len);
  def->group = grp;
  def->npes = npes;
  for (i = 0; i < npes; i++)
    def->pes[i] = pes[i];
  CmiSetHandler(def, CmiGroupHandlerIndex);
  CmiGroupHandler(def);
  CmiSyncBroadcastAndFree(len, def);
  return grp;
}

void CmiLookupGroup(CmiGroup grp, int *npes, int **pes) {
  unsigned int hashval, bucket;
  GroupDef def;
  GroupDef *table = CmiGroupTable;
  hashval = (grp.id ^ grp.pe);
  bucket = hashval % GROUPTAB_SIZE;
  for (def = table[bucket]; def; def = def->next) {
    if ((def->group.id == grp.id) && (def->group.pe == grp.pe)) {
      *npes = def->npes;
      *pes = def->pes;
      return;
    }
  }
  *npes = 0;
  *pes = 0;
}

void CmiSyncListSend(int npes, const int *pes, int len, void *msg) {
  for (int i = 0; i < npes; i++) {
    CmiSyncSend(pes[i], len, msg);
  }
}

void CmiSyncListSendFn(int npes, const int *pes, int len, char *msg) {
  CmiSyncListSend(npes, pes, len, (void *)msg);
}

void CmiSyncListSendAndFree(int npes, const int *pes, int len, void *msg) {
  for (int i = 0; i < npes; i++) {
    CmiSyncSend(pes[i], len, msg);
  }
  CmiFree(msg);
}

void CmiFreeListSendFn(int npes, const int *pes, int len, char *msg) {
  CmiSyncListSendAndFree(npes, pes, len, (void *)msg);
}

void CmiSyncMulticast(CmiGroup grp, int size, void *msg) {
  int i, *pes;
  int npes;
  CmiLookupGroup(grp, &npes, &pes);
  if (npes == 0) {
    CmiAbort("CmiSyncMulticast: group not found\n");
  }
  CmiSyncListSend(npes, (const int *)pes, size, msg);
}

void CmiSyncMulticastAndFree(CmiGroup grp, int size, void *msg) {
  CmiSyncMulticast(grp, size, msg);
  CmiFree(msg);
}

void CmiSyncMulticastFn(CmiGroup grp, int size, char *msg) {
  CmiSyncMulticast(grp, size, (void *)msg);
}

void CmiFreeMulticastFn(CmiGroup grp, int size, char *msg) {
  CmiSyncMulticastAndFree(grp, size, (void *)msg);
}
