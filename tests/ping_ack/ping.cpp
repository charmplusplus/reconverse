#include <stdio.h>
#include <math.h>
#include <time.h>
#include "converse.h"

CpvDeclare(int, bigmsg_index);
CpvDeclare(int, ackmsg_index);
CpvDeclare(int, shortmsg_index);
CpvDeclare(int, msg_size);
CpvDeclare(int, trial);               // increments per trial, gets set to 0 at the start of a new msg size
CpvDeclare(int, round);               // increments per msg size
CpvDeclare(int, warmup_flag);         // 1 when in warmup round, 0 when not
CpvDeclare(int, recv_count);
CpvDeclare(int, ack_count);
CpvDeclare(double, total_time);
CpvDeclare(double, process_time);
CpvDeclare(double, send_time);

int msg_count;
#define MAX_MSG_SIZES 32              // upper bound on the number of sizes in the sweep
#define DEFAULT_MIN_SIZE 16           // smallest payload (bytes), overridden by -min_size
#define DEFAULT_MAX_SIZE 2048         // largest payload (bytes), overridden by -max_size
#define DEFAULT_TRIALS 100            // iterations per msg size, overridden by -iterations
#define CALCULATION_PRECISION 0.0001  // the decimal place that the output data is rounded to

int nTRIALS_PER_SIZE;                 // iterations run per msg size
int nMSG_SIZE;                        // number of msg sizes in the sweep

double *total_time;                   // times are stored in us, nTRIALS_PER_SIZE entries each
double *process_time;
double *send_time;


int msg_sizes[MAX_MSG_SIZES];         // msg sizes on the wire (payload + header), filled by build_msg_sizes



typedef struct myMsg
{
  char header[CmiMsgHeaderSizeBytes];
  int payload[1];
} *message;

// helper functions

double round_to(double val, double precision) {
  return round(val / precision) * precision;
}

double get_average(double arr[]) {
  double tot = 0;
  for (int i = 0; i < nTRIALS_PER_SIZE; ++i) tot += arr[i];
  return (round_to(tot, CALCULATION_PRECISION) / nTRIALS_PER_SIZE);

}

double get_stdev(double arr[]) {
  double stdev = 0.0;
  double avg = get_average(arr);
  for (int i = 0; i < nTRIALS_PER_SIZE; ++i)
    stdev += pow(arr[i] - avg, 2);
  stdev = sqrt(stdev / nTRIALS_PER_SIZE);
  return stdev;
}

double get_max(double arr[]) {
  double max = arr[0];
  for (int i = 1; i < nTRIALS_PER_SIZE; ++i)
                if (arr[i] > max) max = arr[i];
        return max;
}

// Fills msg_sizes with payload sizes doubling from min_size up to max_size, adding the
// converse header to each. Returns the number of sizes generated.
int build_msg_sizes(int min_size, int max_size) {
  int n = 0;
  int size = min_size;
  while (n < MAX_MSG_SIZES) {
    msg_sizes[n++] = size + CmiMsgHeaderSizeBytes;
    if (size > max_size / 2) break;  // doubling again would overshoot max_size (and could overflow)
    size *= 2;
  }
  return n;
}


void print_results() {
  if (!CpvAccess(warmup_flag)) {
    CmiPrintf("msg_size=%d\n", CpvAccess(msg_size));
    //for (int i = 0; i < nTRIALS_PER_SIZE; ++i) {
      //DEBUG: print without trial number:
      //CmiPrintf("Send time: %f, process time: %f, total time: %f\n", send_time[i], process_time[i], total_time[i]);

      //DEBUG: print with trial number:
      //CmiPrintf("%d %f\n  %f\n  %f\n", i, send_time[i], process_time[i], total_time[i]);
    //}
    // print data:
    CmiPrintf("Format: {#PEs},{msg_size},{average send/process/total time (us)},{stdevs*3},{maxs*3}\n");
    CmiPrintf("DATA,%d,%d,%f,%f,%f,%f,%f,%f,%f,%f,%f\n", CmiNumPes(), CpvAccess(msg_size), get_average(send_time), get_average(process_time), get_average(total_time),
                                get_stdev(send_time), get_stdev(process_time), get_stdev(total_time), get_max(send_time), get_max(process_time), get_max(total_time));
                
                
  } else {
    if (CpvAccess(round) == nMSG_SIZE - 1)  // if this is the end of the warmup round
      CmiPrintf("Warm up Done!\n");

    // DEBUG: Print what msg_size the warmup round is on
    // else                               // otherwise move to the next msg size
    //  CmiPrintf("Warming up msg_size %d\n", CpvAccess(msg_size));
  }
}

void send_msg() {
  double start_time, crt_time;
  struct myMsg *msg;
  //  CmiPrintf("\nSending msg fron pe%d to pe%d\n",CmiMyPe(), CmiNumPes()/2+CmiMyPe());
  CpvAccess(process_time) = 0.0;
  CpvAccess(send_time) = 0.0;
  CpvAccess(total_time) = CmiWallTimer();
  for(int k = 0; k < msg_count; k++) {
    crt_time = CmiWallTimer();
    msg = (message)CmiAlloc(CpvAccess(msg_size));

    // Fills payload with ints
    for (int i = 0; i < (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int); ++i) msg->payload[i] = i;
    
    // DEBUG: Print ints stored in payload
    // for (int i = 0; i < (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int); ++i) CmiPrintf("%d ", msg->payload[i]);
    // CmiPrintf("\n");

    CmiSetHandler(msg, CpvAccess(bigmsg_index));
    CpvAccess(process_time) = CmiWallTimer() - crt_time + CpvAccess(process_time);
    start_time = CmiWallTimer();
    //Send from my pe-i on node-0 to q+i on node-1
    CmiSyncSendAndFree(CmiNumPes() / 2 + CmiMyPe(), CpvAccess(msg_size), msg);
    CpvAccess(send_time) = CmiWallTimer() - start_time + CpvAccess(send_time);
  }
}

void shortmsg_handler(void *vmsg) {
  message smsg = (message)vmsg;
  CmiFree(smsg);
  if (!CpvAccess(warmup_flag)) {     // normal round handling
    if (CpvAccess(trial) == nTRIALS_PER_SIZE) { // if we have run the current msg size for nTRIALS
      CpvAccess(round) = CpvAccess(round) + 1;
      CpvAccess(trial) = 0;
      CpvAccess(msg_size) = msg_sizes[CpvAccess(round)];
    } 
  } else {   // warmup round handling
    if (CpvAccess(round) == nMSG_SIZE - 1) {  // if this is the end of the warmup round
      CpvAccess(round) = 0;
      CpvAccess(msg_size) = msg_sizes[0];
      CpvAccess(warmup_flag) = 0;
    } else {                                  // otherwise warm up the next msg size
      CpvAccess(round) = CpvAccess(round) + 1;
      CpvAccess(msg_size) = msg_sizes[CpvAccess(round)];
    }
    CpvAccess(trial) = 0;
  }
  send_msg();
}

void do_work(long start, long end, void *result) {
  long tmp=0;
  for (long i=start; i<=end; i++) {
    tmp+=(long)(sqrt(1+cos(i*1.57)));
  }
  *(long *)result = tmp + *(long *)result;
}


void bigmsg_handler(void *vmsg)
{
  int i, next;
  message msg = (message)vmsg;
  // if this is a receiving PE
  if (CmiMyPe() >= CmiNumPes() / 2) {
    CpvAccess(recv_count) = 1 + CpvAccess(recv_count);
    long sum = 0;
    long result = 0;
    double num_ints = (CpvAccess(msg_size) - CmiMsgHeaderSizeBytes) / sizeof(int);
    double exp_avg = (num_ints - 1) / 2;
    for (i = 0; i < num_ints; ++i) {
      sum += msg->payload[i];
      do_work(i,sum,&result);
    }
    if(result < 0) {
      CmiPrintf("Error! in computation");
    }
    double calced_avg = sum / num_ints;
    if (calced_avg != exp_avg) {
      CmiPrintf("Calculated average of %f does not match expected value of %f, exiting\n", calced_avg, exp_avg);
      CmiExit(1);
    } 
    // else
    //   CmiPrintf("Calculation OK\n"); // DEBUG: Computation Check
    if(CpvAccess(recv_count) == msg_count) {
      CpvAccess(recv_count) = 0;
      
      CmiFree(msg);
      msg = (message)CmiAlloc(CpvAccess(msg_size));
      CmiSetHandler(msg, CpvAccess(ackmsg_index));
      CmiSyncSendAndFree(0, CpvAccess(msg_size), msg);
    } else
      CmiFree(msg);
  } else
    CmiPrintf("\nError: Only node-1 can be receiving node!!!!\n");
}

void pe0_ack_handler(void *vmsg)
{
  int pe;
  message msg = (message)vmsg;
   //Pe-0 receives all acks
  CpvAccess(ack_count) = 1 + CpvAccess(ack_count);

  // DEBUG: Computation Print Check
  // CmiPrintf("All %d messages of size %d on trial %d OK\n", MSG_COUNT, CpvAccess(msg_size), CpvAccess(trial));
    

  if(CpvAccess(ack_count) == CmiNumPes()/2) {
    CpvAccess(ack_count) = 0;
    CpvAccess(total_time) = CmiWallTimer() - CpvAccess(total_time);

    // DEBUG: Original Print Statement
    //CmiPrintf("Received [Trial=%d, msg size=%d] ack on PE-#%d send time=%lf, process time=%lf, total time=%lf\n",
    //         CpvAccess(trial), CpvAccess(msg_size), CmiMyPe(), CpvAccess(send_time), CpvAccess(process_time), CpvAccess(total_time));

    CmiFree(msg);

    // store times in arrays
    send_time[CpvAccess(trial)] =  CpvAccess(send_time) * 1000000.0;       // convert to microsecs.
    process_time[CpvAccess(trial)] = CpvAccess(process_time) * 1000000.0;
    total_time[CpvAccess(trial)] = CpvAccess(total_time) * 1000000.0;

    CpvAccess(trial) = CpvAccess(trial) + 1;

    // print results
    if (CpvAccess(warmup_flag) || CpvAccess(trial) == nTRIALS_PER_SIZE) print_results();

    // if this is not the warmup round, and we have finished the final trial, and we are on the final msg size, exit
    if(!CpvAccess(warmup_flag) && CpvAccess(trial) == nTRIALS_PER_SIZE && CpvAccess(round) == nMSG_SIZE - 1)
      CmiExit(0);
    else {
      // CmiPrintf("\nSending short msgs from PE-%d", CmiMyPe());
      for(pe = 0 ; pe<CmiNumPes() / 2; pe++) {
        int smsg_size = 4+CmiMsgHeaderSizeBytes;
        message smsg = (message)CmiAlloc(smsg_size);
        CmiSetHandler(smsg, CpvAccess(shortmsg_index));
        CmiSyncSendAndFree(pe, smsg_size, smsg);
      }
    }
  }
}

void bigmsg_moduleinit(int argc, char **argv)
{
  CpvInitialize(int, bigmsg_index);
  CpvInitialize(int, ackmsg_index);
  CpvInitialize(int, shortmsg_index);
  CpvInitialize(int, msg_size);
  CpvInitialize(int, trial);
  CpvInitialize(int, round);
  CpvInitialize(int, warmup_flag);
  CpvInitialize(int, recv_count);
  CpvInitialize(int, ack_count);
  CpvInitialize(double, total_time);
  CpvInitialize(double, send_time);
  CpvInitialize(double, process_time);

  CpvAccess(bigmsg_index) = CmiRegisterHandler(bigmsg_handler);
  CpvAccess(shortmsg_index) = CmiRegisterHandler(shortmsg_handler);
  CpvAccess(ackmsg_index) = CmiRegisterHandler(pe0_ack_handler);
  CpvAccess(trial) = 0;
  CpvAccess(round) = 0;
  CpvAccess(warmup_flag) = 1;
  msg_count = 100; // default msg count
  CmiGetArgInt(argv, "-msg_count", &msg_count);

  int min_size = DEFAULT_MIN_SIZE;
  int max_size = DEFAULT_MAX_SIZE;
  nTRIALS_PER_SIZE = DEFAULT_TRIALS;
  CmiGetArgInt(argv, "-min_size", &min_size);
  CmiGetArgInt(argv, "-max_size", &max_size);
  CmiGetArgInt(argv, "-iterations", &nTRIALS_PER_SIZE);

  if (min_size < (int)sizeof(int) || max_size < min_size || nTRIALS_PER_SIZE < 1) {
    if (CmiMyPe() == 0)
      CmiPrintf("Error: need -min_size >= %d, -max_size >= min_size, and -iterations >= 1 "
                "(got min_size=%d, max_size=%d, iterations=%d), exiting\n",
                (int)sizeof(int), min_size, max_size, nTRIALS_PER_SIZE);
    CmiExit(1);
  }

  nMSG_SIZE = build_msg_sizes(min_size, max_size);
  int largest = msg_sizes[nMSG_SIZE - 1] - CmiMsgHeaderSizeBytes;
  if (CmiMyPe() == 0 && largest <= max_size / 2)
    CmiPrintf("note: sweep truncated to the %d size limit, largest payload is %d bytes\n",
              MAX_MSG_SIZES, largest);

  CpvAccess(msg_size) = msg_sizes[0];

  // only PE-0 records and reports the timings
  if (CmiMyPe() == 0) {
    total_time = new double[nTRIALS_PER_SIZE];
    process_time = new double[nTRIALS_PER_SIZE];
    send_time = new double[nTRIALS_PER_SIZE];
    CmiPrintf("Running %d iterations of %d msgs for payloads %d..%d bytes (%d sizes)\n",
              nTRIALS_PER_SIZE, msg_count, min_size, largest, nMSG_SIZE);
  }


  // Set runtime cpuaffinity
  CmiInitCPUAffinity(argv);
  // Initialize CPU topology
  //CmiInitCPUTopology(argv);
  if (CmiMyPe()==0 && CmiNumPes()%2 != 0) {
    CmiPrintf("note: this test requires at multiple of 2 pes, skipping test.\n");
    CmiPrintf("exiting.\n");
    //CsdExitScheduler();
    CmiExit(1);
  }

  // Wait for all PEs of the node to complete topology init
  CmiNodeAllBarrier();

  // Update the argc after runtime parameters are extracted out
  argc = CmiGetArgc(argv);
  if(CmiMyPe() < CmiNumPes()/2)
    send_msg();
}

int main(int argc, char **argv)
{
	ConverseInit(argc,argv,bigmsg_moduleinit,0,0);
}