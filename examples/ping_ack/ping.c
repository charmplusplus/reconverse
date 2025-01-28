#include <stdio.h>
#include <math.h>
#include <time.h>
#include <converse.h>
#include "ping.cpm.h"

CpvDeclare(int, ping_index);
CpvDeclare(int, ackmsg_index);
CpvDeclare(int, msg_size);
CpvDeclare(int, ack_count);
CpvDeclare(double, total_time);
CpvDeclare(double, process_time);
CpvDeclare(double, send_time);

typedef struct myMsg
{
  char header[CmiMsgHeaderSizeBytes];
  int payload[1];
} *message;



void print_results() {
  CmiPrintf("msg_size\n%d\n", CpvAccess(msg_size));
}

CpmInvokable ping_stop()
{
  CsdExitScheduler();
}



void send_msg() {
  double start_time, crt_time;
  struct myMsg *msg;
  msg = (message)CmiAlloc(CpvAccess(msg_size));
  // Fills payload with ints
  for (int i = 0; i < (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int); ++i) msg->payload[i] = i;
  
  // DEBUG: Print ints stored in payload
  // for (int i = 0; i < (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int); ++i) CmiPrintf("%d ", msg->payload[i]);
  // CmiPrintf("\n");

  CmiSetHandler(msg, CpvAccess(ping_index));
  //Send from my pe-i on node-0 to q+i on node-1
  CmiSyncSendAndFree(CmiNumPes() / 2 + CmiMyPe(), CpvAccess(msg_size), msg);
}


void ping_handler(void *vmsg)
{
  int i, next;
  message msg = (message)vmsg;
  // if this is a receiving PE
  if (CmiMyPe() >= CmiNumPes() / 2) {
    long sum = 0;
    long result = 0;
    double num_ints = (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int);
    double exp_avg = (num_ints - 1) / 2;
    for (i = 0; i < num_ints; ++i) {
      sum += msg->payload[i];
    }
    if(result < 0) {
      CmiPrintf("Error! in computation");
    }
    double calced_avg = sum / num_ints;
    if (calced_avg != exp_avg) {
      CmiPrintf("Calculated average of %f does not match expected value of %f, exiting\n", calced_avg, exp_avg);
      Cpm_ping_stop(CpmSend(CpmALL)); 
    } 
    // else
    //   CmiPrintf("Calculation OK\n"); // DEBUG: Computation Check
      
    CmiFree(msg);
    msg = (message)CmiAlloc(CpvAccess(msg_size));
    CmiSetHandler(msg, CpvAccess(ackmsg_index));
    CmiSyncSendAndFree(0, CpvAccess(msg_size), msg);
  } else
    CmiPrintf("\nError: Only node-1 can be receiving node!!!!\n");
}

void pe0_ack_handler(void *vmsg)
{
  int pe;
  message msg = (message)vmsg;
   //Pe-0 receives all acks
  CpvAccess(ack_count) = 1 + CpvAccess(ack_count);

  if(CpvAccess(ack_count) == CmiNumPes()/2) {
    CpvAccess(ack_count) = 0;

    CmiFree(msg);

    // print results
    print_results();

    Cpm_ping_stop(CpmSend(CpmALL));
  }
}


void ping_init()
{
  int totalpes = CmiNumPes(); //p=num_pes
  int pes_per_node = totalpes/2; //q=p/2
  if (CmiNumPes()%2 !=0) {
    CmiPrintf("note: this test requires at multiple of 2 pes, skipping test.\n");
    CmiPrintf("exiting.\n");
    CsdExitScheduler();
    Cpm_ping_stop(CpmSend(CpmALL));
  } else {
    if(CmiMyPe() < pes_per_node)
      send_msg();
  }
}



void ping_moduleinit(int argc, char **argv)
{
  CpvInitialize(int, ping_index);
  CpvInitialize(int, ackmsg_index);
  CpvInitialize(int, msg_size);
  CpvInitialize(int, ack_count);

  CpvAccess(ping_index) = CmiRegisterHandler(ping_handler);
  CpvAccess(ackmsg_index) = CmiRegisterHandler(pe0_ack_handler);
  CpvAccess(msg_size) = 16+CmiMsgHeaderSizeBytes+100;
  void CpmModuleInit(void);
  void CfutureModuleInit(void);
  void CpthreadModuleInit(void);

  CpmModuleInit();
  CfutureModuleInit();
  CpthreadModuleInit();
  CpmInitializeThisModule();
  // Set runtime cpuaffinity
  CmiInitCPUAffinity(argv);
  // Initialize CPU topology
  CmiInitCPUTopology(argv);
  // Wait for all PEs of the node to complete topology init
        CmiNodeAllBarrier();

  // Update the argc after runtime parameters are extracted out
  argc = CmiGetArgc(argv);
  if(CmiMyPe() < CmiNumPes()/2)
    ping_init();
}

int main(int argc, char **argv)
{
	ConverseInit(argc,argv,ping_moduleinit,0,0);
}
