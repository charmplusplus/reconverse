// A send must return although its destination is not receiving.
//
// Three processes of one PE each. PE 2 registers a small buffer and tells
// PE 0. On PE 0's go-ahead PE 1 stays in one handler for `block` seconds, in
// a loop that calls nothing of Converse: its process makes no network
// progress, as a process does that is inside a long computation or blocked in
// another library. Meanwhile PE 0 sends PE 1 `count` messages from one
// handler, of sizes on both sides of the backend's eager limit and far more
// in total than the network buffers between the two processes hold, so the
// network layer stops accepting them. PE 0 then issues a zero-copy get from
// PE 2's buffer and sends itself a message. PE 2 is receiving, so the get
// completes while PE 1 is still busy, and its completion callback, which the
// backend calls from inside its network progress, sends PE 1 one more
// message.
//
// Checked:
//  - PE 0 gets back from all its sends, runs the handler of the message to
//    itself, and gets back from the send in the callback, all within half of
//    `block`. A backend that retries a send until the network takes it stays
//    in the send loop until PE 1 resumes.
//  - PE 1 then receives every message exactly once, with the size and the
//    contents it was sent with, the one sent from the callback included.
//
// Usage: send_nonblocking [count = 10000] [block seconds = 6]
#include "conv-rdma.h"
#include "converse.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static const int sizes[] = {64, 4000, 8000, 20000, 70000};
static const int nsizes = sizeof(sizes) / sizeof(sizes[0]);
static const size_t kGetBytes = 4096;

static int ready_handler, block_handler, data_handler, after_handler,
    done_handler, exit_handler;
static long count = 10000;
static double block_seconds = 6.0;

struct Msg {
  char core[CmiMsgHeaderSizeBytes];
  long seq;
  CmiNcpyBuffer buf;  // ready message only
};

// PE 0
static double send_start, send_loop_end = -1, after_time = -1, get_done = -1;
static CmiNcpyBuffer local_buf, remote_buf;
// PE 1
static std::vector<char> seen;
static long received = 0;

static int sizeOf(long seq) { return seq == count ? 64 : sizes[seq % nsizes]; }

static unsigned char patternByte(long seq, size_t i) {
  return (unsigned char)((i * 131u + (size_t)seq * 17u + 7u) & 0xffu);
}

static Msg *newMsg(int handler, int bytes = sizeof(Msg)) {
  Msg *m = (Msg *)CmiAlloc(bytes);
  CmiSetHandler(m, handler);
  return m;
}

static void sendData(long seq) {
  int bytes = sizeOf(seq);
  Msg *m = newMsg(data_handler, bytes);
  m->seq = seq;
  char *p = (char *)m;
  for (size_t i = offsetof(Msg, buf); i < (size_t)bytes; i++)
    p[i] = (char)patternByte(seq, i);
  CmiSyncSendAndFree(1, bytes, m);
}

// PE 0: completion of the get, called by the backend from its progress
static void getAck(void *) {
  get_done = CmiWallTimer() - send_start;
  sendData(count);
}

// PE 0: PE 2 has its buffer registered
static void readyHandler(void *vmsg) {
  Msg *msg = (Msg *)vmsg;
  remote_buf = msg->buf;
  CmiFree(msg);
  CmiSyncSendAndFree(1, sizeof(Msg), newMsg(block_handler));

  send_start = CmiWallTimer();
  for (long seq = 0; seq < count; seq++)
    sendData(seq);
  send_loop_end = CmiWallTimer() - send_start;
  char *dest = (char *)CmiAlloc(kGetBytes);
  local_buf = CmiNcpyBuffer(dest, kGetBytes);
  local_buf.rdmaGet(remote_buf, 0, nullptr, nullptr);
  CmiSyncSendAndFree(0, sizeof(Msg), newMsg(after_handler));
}

// PE 0: the first handler after the one that sent
static void afterHandler(void *msg) {
  CmiFree(msg);
  after_time = CmiWallTimer() - send_start;
}

// PE 1: stop receiving for a while
static void blockHandler(void *msg) {
  CmiFree(msg);
  auto end = std::chrono::steady_clock::now() +
             std::chrono::duration<double>(block_seconds);
  volatile unsigned long spin = 0;
  while (std::chrono::steady_clock::now() < end)
    spin = spin + 1;
}

// PE 1
static void dataHandler(void *vmsg) {
  Msg *m = (Msg *)vmsg;
  long seq = m->seq;
  if (seq < 0 || seq > count)
    CmiAbort("send_nonblocking: message with sequence number %ld\n", seq);
  if (seen[seq])
    CmiAbort("send_nonblocking: message %ld arrived twice\n", seq);
  seen[seq] = 1;
  int bytes = sizeOf(seq);
  const char *p = (const char *)m;
  for (size_t i = offsetof(Msg, buf); i < (size_t)bytes; i++)
    if ((unsigned char)p[i] != patternByte(seq, i))
      CmiAbort("send_nonblocking: message %ld (%d bytes): byte %zu is wrong\n",
               seq, bytes, i);
  CmiFree(m);
  if (++received == count + 1)
    CmiSyncSendAndFree(0, sizeof(Msg), newMsg(done_handler));
}

// PE 0: PE 1 has everything
static void doneHandler(void *msg) {
  CmiFree(msg);
  CmiPrintf("send_nonblocking: %ld messages while the destination was busy "
            "for %.1f s: send loop returned after %.3f s, next handler ran "
            "after %.3f s, get completed after %.3f s; all received intact\n",
            count + 1, block_seconds, send_loop_end, after_time, get_done);
  if (after_time < 0 || after_time > block_seconds / 2 || get_done < 0 ||
      get_done > block_seconds / 2)
    CmiAbort("send_nonblocking: FAIL: the sender was held for %.3f s (limit "
             "%.3f s) by a destination that was not receiving\n",
             after_time > get_done ? after_time : get_done, block_seconds / 2);
  CmiPrintf("send_nonblocking: PASS\n");
  CmiSyncBroadcastAllAndFree(sizeof(Msg), newMsg(exit_handler));
}

static void exitHandler(void *msg) {
  CmiFree(msg);
  CsdExitScheduler();
}

static void start(int argc, char **argv) {
  ready_handler = CmiRegisterHandler(readyHandler);
  block_handler = CmiRegisterHandler(blockHandler);
  data_handler = CmiRegisterHandler(dataHandler);
  after_handler = CmiRegisterHandler(afterHandler);
  done_handler = CmiRegisterHandler(doneHandler);
  exit_handler = CmiRegisterHandler(exitHandler);
  CmiSetDirectNcpyAckHandler(getAck);
  argc = CmiGetArgc(argv);
  if (argc > 1) count = atol(argv[1]);
  if (argc > 2) block_seconds = atof(argv[2]);

  if (CmiNumPes() != 3 || CmiNumNodes() != 3)
    CmiAbort("send_nonblocking needs 3 PEs in 3 processes: <launcher> -n 3 "
             "./reconverse_send_nonblocking +pe 3\n");
  // every PE has its handlers before any sends
  CmiBarrier();

  if (CmiMyPe() == 1) seen.assign(count + 1, 0);
  if (CmiMyPe() == 2) {
    char *src = (char *)CmiAlloc(kGetBytes);
    memset(src, 0x5a, kGetBytes);
    Msg *m = newMsg(ready_handler);
    m->buf = CmiNcpyBuffer(src, kGetBytes);
    CmiSyncSendAndFree(0, sizeof(Msg), m);
  }
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, start, 0, 0);
  return 0;
}
