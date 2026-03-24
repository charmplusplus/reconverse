/* GPU device RDMA pingpong benchmark
 * Mirrors rdma_pingpong/pingpong.cpp but allocates buffers in GPU device
 * memory.  Within-process transfers use cudaMemcpy / hipMemcpy (the
 * memcpyAnyPtr path in conv-rdma.cpp); cross-process transfers use LCI's
 * GPUDirect path, which auto-detects device pointers inside registerMemory.
 *
 * Build with -DRECONVERSE_ENABLE_CUDA=ON or -DRECONVERSE_ENABLE_HIP=ON.
 */
#include "conv-rdma.h"
#include <converse.h>
#include <cstdio>
#include <cstdlib>

#include "converse_config.h"

#if CMK_CUDA
#include <cuda_runtime.h>
#define GPU_MALLOC(pptr, sz)    cudaMalloc((pptr), (sz))
#define GPU_FREE(ptr)           cudaFree(ptr)
#define GPU_MEMSET(ptr, v, sz)  cudaMemset((ptr), (v), (sz))
#define GPU_D2H(dst, src, sz)   cudaMemcpy((dst), (src), (sz), cudaMemcpyDeviceToHost)
#define GPU_GET_DEVICE(d)       cudaGetDevice(d)
#elif CMK_HIP
#include <hip/hip_runtime.h>
#define GPU_MALLOC(pptr, sz)    hipMalloc((pptr), (sz))
#define GPU_FREE(ptr)           hipFree(ptr)
#define GPU_MEMSET(ptr, v, sz)  hipMemset((ptr), (v), (sz))
#define GPU_D2H(dst, src, sz)   hipMemcpy((dst), (src), (sz), hipMemcpyDeviceToHost)
#define GPU_GET_DEVICE(d)       hipGetDevice(d)
#else
#error "pingpong_device requires CMK_CUDA or CMK_HIP. " \
       "Build reconverse with RECONVERSE_ENABLE_CUDA=ON or RECONVERSE_ENABLE_HIP=ON."
#endif

// ------------------------------------------------------------------ state ---

CpvDeclare(int, nCycles);
CpvDeclare(int, minMsgSize);
CpvDeclare(int, maxMsgSize);
CpvDeclare(int, factor);
CpvDeclare(bool, warmUp);
CpvDeclare(int, msgSize);
CpvDeclare(int, currentCycle);

CpvDeclare(void *, deviceBuf);   // raw device allocation (owned by this PE)
CpvDeclare(CmiNcpyBuffer, localBuf);
CpvDeclare(CmiNcpyBuffer, remoteBuf);
CpvStaticDeclare(double, startTime);
CpvStaticDeclare(double, endTime);

CpvDeclare(int, setRemoteBufHIdx);
CpvDeclare(int, rmaGetSignalHIdx);
CpvDeclare(int, exitBenchmarkHIdx);

// ------------------------------------------------------- buffer lifecycle ---

void setupRMABuf();

void startRing() {
  CmiAssert(CmiMyPe() == 0);
  setupRMABuf();
}

void setupRMABuf() {
  // Release the previous device buffer if one exists
  if (CpvAccess(deviceBuf) != nullptr) {
    CpvAccess(localBuf).deregisterMem();
    GPU_FREE(CpvAccess(deviceBuf));
    CpvAccess(deviceBuf) = nullptr;
  }

  // Allocate a new device buffer and fill it with the local PE id so the
  // receiver can verify the data came from the right place.
  void *dbuf = nullptr;
  GPU_MALLOC(&dbuf, CpvAccess(msgSize));
  GPU_MEMSET(dbuf, CmiMyPe(), CpvAccess(msgSize));
  CpvAccess(deviceBuf) = dbuf;

  // Register with the Converse RDMA layer (CmiNcpyBuffer calls
  // CmiSetRdmaBufferInfo → comm_backend::registerMemory, which hands the
  // device pointer to LCI; LCI detects it as a device allocation and
  // registers it with GPUDirect-aware MR flags).
  CpvAccess(localBuf) = CmiNcpyBuffer(dbuf, CpvAccess(msgSize));

  // Exchange buffer descriptors with the other PE
  char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer));
  *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes)) = CpvAccess(localBuf);
  CmiSetHandler(msg, CpvAccess(setRemoteBufHIdx));
  CmiSyncSendAndFree(1 - CmiMyPe(),
                     CmiMsgHeaderSizeBytes + sizeof(CmiNcpyBuffer), msg);
}

// --------------------------------------------------- handler declarations ---

void startWarmUp();
void doRMA();
void endRing();
void exitBenchmark();

// ------------------------------------------------------------ AM handlers ---

void setRemoteBufHandler(char *msg) {
  CpvAccess(remoteBuf) = *((CmiNcpyBuffer *)(msg + CmiMsgHeaderSizeBytes));
  CmiFree(msg);
  if (CmiMyPe() == 1) {
    // PE 1 mirrors the message size chosen by PE 0, then sends its own
    // descriptor back so PE 0 can also pull from PE 1's buffer.
    CpvAccess(msgSize) = CpvAccess(remoteBuf).cnt;
    setupRMABuf();
  } else {
    startWarmUp();
  }
}

void rmaGetSignalHandler(char *msg) {
  CmiFree(msg);
  if (CmiMyPe() == 1) {
    // PE 1 always just does the "pong" pull
    doRMA();
    return;
  }
  // PE 0 drives the measurement loop
  if (CpvAccess(warmUp)) {
    CpvAccess(warmUp) = false;
    CpvAccess(startTime) = CmiWallTimer();
    CpvAccess(currentCycle) = 0;
    doRMA();
  } else {
    CpvAccess(currentCycle) += 1;
    if (CpvAccess(currentCycle) < CpvAccess(nCycles))
      doRMA();
    else
      endRing();
  }
}

void exitBenchmarkHandler(char *msg) {
  CmiFree(msg);
  // Release device memory
  if (CpvAccess(deviceBuf) != nullptr) {
    CpvAccess(localBuf).deregisterMem();
    GPU_FREE(CpvAccess(deviceBuf));
    CpvAccess(deviceBuf) = nullptr;
  }
  CsdExitScheduler();
}

// -------------------------------------------------- benchmark operations ---

void startWarmUp() {
  CmiAssert(CmiMyPe() == 0);
  CpvAccess(warmUp) = true;
  doRMA();
}

// PE 0 pulls from PE 1's device buffer (Rget).  The same call is used for
// the same-process case (memcpyAnyPtr) and the cross-process case (LCI
// GPUDirect), chosen transparently by the RDMA layer.
void doRMA() {
  CpvAccess(localBuf).rdmaGet(CpvAccess(remoteBuf), 0, nullptr, nullptr);
}

// Invoked by the Converse RDMA layer on local transfer completion.
// context is a NcpyOperationInfo*.
void rmaGetLocalComp(void *context) {
  NcpyOperationInfo *info = (NcpyOperationInfo *)context;
  if (CmiMyPe() == info->srcPe)
    return;  // source side: no action needed

  // Destination side: notify the source (and the initiator of pong) that
  // the transfer is done so the next round can proceed.
  char *sig = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(sig, CpvAccess(rmaGetSignalHIdx));
  CmiSyncSendAndFree(1 - CmiMyPe(), CmiMsgHeaderSizeBytes, sig);
}

void endRing() {
  CmiAssert(CmiMyPe() == 0);
  CpvAccess(endTime) = CmiWallTimer();

  double us_one_way = (1e6 * (CpvAccess(endTime) - CpvAccess(startTime))) /
                      (2.0 * CpvAccess(nCycles));
  CmiPrintf("[GPU] Size=%zu bytes, time=%.3lf us one-way\n",
            CpvAccess(msgSize), us_one_way);

  if (CpvAccess(msgSize) < CpvAccess(maxMsgSize)) {
    CpvAccess(msgSize) *= CpvAccess(factor);
    startRing();
  } else {
    exitBenchmark();
  }
}

void exitBenchmark() {
  char *msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes);
  CmiSetHandler(msg, CpvAccess(exitBenchmarkHIdx));
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes, msg);
}

// ------------------------------------------------------------- Cmi entry ---

CmiStartFn mymain(int argc, char *argv[]) {
  CpvInitialize(int, nCycles);
  CpvInitialize(int, minMsgSize);
  CpvInitialize(int, maxMsgSize);
  CpvInitialize(int, factor);
  CpvInitialize(bool, warmUp);
  CpvInitialize(int, msgSize);
  CpvInitialize(int, currentCycle);
  CpvInitialize(void *, deviceBuf);
  CpvInitialize(CmiNcpyBuffer, localBuf);
  CpvInitialize(CmiNcpyBuffer, remoteBuf);
  CpvInitialize(double, startTime);
  CpvInitialize(double, endTime);

  CpvAccess(deviceBuf) = nullptr;

  CpvInitialize(int, setRemoteBufHIdx);
  CpvAccess(setRemoteBufHIdx) =
      CmiRegisterHandler((CmiHandler)setRemoteBufHandler);
  CpvInitialize(int, rmaGetSignalHIdx);
  CpvAccess(rmaGetSignalHIdx) =
      CmiRegisterHandler((CmiHandler)rmaGetSignalHandler);
  CpvInitialize(int, exitBenchmarkHIdx);
  CpvAccess(exitBenchmarkHIdx) =
      CmiRegisterHandler((CmiHandler)exitBenchmarkHandler);

  CmiInitCPUAffinity(argv);
  CmiInitCPUTopology(argv);
  CmiNodeAllBarrier();

  argc = CmiGetArgc(argv);
  if (argc == 5) {
    CpvAccess(nCycles) = atoi(argv[1]);
    CpvAccess(minMsgSize) = atoi(argv[2]);
    CpvAccess(maxMsgSize) = atoi(argv[3]);
    CpvAccess(factor) = atoi(argv[4]);
  } else if (argc == 1) {
    CpvAccess(nCycles) = 100;
    CpvAccess(minMsgSize) = 1 << 9;
    CpvAccess(maxMsgSize) = 1 << 14;
    CpvAccess(factor) = 2;
  } else {
    if (CmiMyPe() == 0)
      CmiAbort("Usage: ./pingpong_device [<ncycles> <minsize> <maxsize> "
               "<factor>]\n");
  }

  if (CmiNumPes() != 2 && CmiMyPe() == 0)
    CmiAbort("This test requires exactly 2 PEs (+p 2 or two nodes)\n");

  // Report which GPU each PE is bound to
  int dev = -1;
  GPU_GET_DEVICE(&dev);
  CmiPrintf("PE %d using GPU device %d\n", CmiMyPe(), dev);

  if (CmiMyPe() == 0)
    CmiPrintf("[GPU pingpong] cycles=%d  min=%d  max=%d  factor=%d\n",
              CpvAccess(nCycles), CpvAccess(minMsgSize),
              CpvAccess(maxMsgSize), CpvAccess(factor));

  CpvAccess(msgSize) = CpvAccess(minMsgSize);
  CmiSetDirectNcpyAckHandler(rmaGetLocalComp);

  CmiNodeBarrier();

  if (CmiMyPe() == 0)
    startRing();

  return 0;
}

int main(int argc, char *argv[]) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
