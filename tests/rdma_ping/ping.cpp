#include "converse.h"
#include <pthread.h>
#include <stdio.h>
// #include "ping.cpm.h"

CpvDeclare(int, ping_index);
CpvDeclare(int, ackmsg_index);
CpvDeclare(int, stop_index);
CpvDeclare(int, msg_size);
CpvDeclare(int, ack_count);

struct Message {
  CmiMessageHeader header;
  int *payload;
};

void print_results() { printf("msg_size = %d\n", CpvAccess(msg_size)); }

// CpmInvokable ping_stop()
void ping_stop_handler(void *msg) {
  CmiFree(msg);
  printf("Stopping PE %d\n", CmiMyPe());
  CsdExitScheduler();
}

void send_msg() {
  int destPE = CmiMyPe() + CmiNumPes() / 2;
  void *msg = CmiAlloc(CpvAccess(msg_size));
  CmiSetHandler(msg, CpvAccess(ping_index));
  // payload
  int payload_size = CpvAccess(msg_size) - CmiMsgHeaderSizeBytes;
  printf("total payload bytes = %d\n", payload_size);
  int *p_payload = (int *)((char *)msg + CmiMsgHeaderSizeBytes);
  for (int i = 0; i < payload_size / sizeof(int); ++i)
    p_payload[i] = i;
  CmiSyncSendAndFree(destPE, CpvAccess(msg_size), msg);
}

void ping_handler(void *msg) {
  int i;
  int *p_payload = (int *)((char *)msg + CmiMsgHeaderSizeBytes);
  // if this is a receiving PE
  if (CmiMyPe() >= CmiNumPes() / 2) {
    long sum = 0;
    long result = 0;
    int num_ints = (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int);
    printf("num_ints=%d\n", num_ints);
    double exp_avg = (num_ints - 1) / 2;
    for (i = 0; i < num_ints; ++i) {
      sum += p_payload[i];
    }
    if (result < 0) {
      printf("Error! in computation");
    }
    double calced_avg = sum / num_ints;
    if (calced_avg != exp_avg) {
      printf("Calculated average of %f does not match expected value of %f, "
             "exiting\n",
             calced_avg, exp_avg);
      CmiExit(1);
      //      Cpm_ping_stop(CpmSend(CpmALL));
    }
    // else
    //   CmiPrintf("Calculation OK\n"); // DEBUG: Computation Check

    CmiFree(msg);
    Message *msg = (Message *)CmiAlloc(sizeof(Message));
    CmiSetHandler(msg, CpvAccess(ackmsg_index));
    /*
    msg = (message)CmiAlloc(CpvAccess(msg_size));
    CmiSetHandler(msg, CpvAccess(ackmsg_index));
    */
    CmiSyncSendAndFree(0, CpvAccess(msg_size), msg);
  } else
    printf("\nError: Only node-1 can be receiving node!!!!\n");
}

void pe0_ack_handler(void *vmsg) {
  int pe;
  Message *msg = (Message *)vmsg;
  // Pe-0 receives all acks
  CpvAccess(ack_count) = 1 + CpvAccess(ack_count);

  if (CpvAccess(ack_count) == CmiNumPes() / 2) {
    CpvAccess(ack_count) = 0;

    CmiFree(msg);

    // print results
    print_results();
    CmiExit(0);
    //    Cpm_ping_stop(CpmSend(CpmALL));
  }
}

void ping_init() {
  if (CmiNumPes() != 2)
    CmiAbort("This test must be run with 2 pes.\n");

  if (CmiMyPe() == 0)
    send_msg();
}

void ping_moduleinit(int argc, char **argv) {
  CpvInitialize(int, ping_index);
  CpvInitialize(int, ackmsg_index);
  CpvInitialize(int, stop_index);
  CpvInitialize(int, msg_size);
  CpvInitialize(int, ack_count);

  CpvAccess(ping_index) = CmiRegisterHandler(ping_handler);
  CpvAccess(ackmsg_index) = CmiRegisterHandler(pe0_ack_handler);
  CpvAccess(stop_index) = CmiRegisterHandler(ping_stop_handler);
  CpvAccess(msg_size) = 16 + sizeof(Message) + 100;
  //  void CpmModuleInit(void);
  //  void CfutureModuleInit(void);
  // void CpthreadModuleInit(void);

  //  CpmModuleInit();
  //  CfutureModuleInit();
  // CpthreadModuleInit();
  //  CpmInitializeThisModule();
  // Set runtime cpuaffinity
  //  CmiInitCPUAffinity(argv);
  // Initialize CPU topology
  //  CmiInitCPUTopology(argv);
  // Wait for all PEs of the node to complete topology init
  CmiNodeBarrier();

  // Update the argc after runtime parameters are extracted out
  // argc = CmiGetArgc(argv);
  if (CmiMyPe() == 0)
    ping_init();
}

int main(int argc, char **argv) { ConverseInit(argc, argv, ping_moduleinit); }
