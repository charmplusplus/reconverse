/* megarecon: the Converse test bank megacon (charm/tests/converse/megacon),
 * ported to reconverse.
 *
 * megacon signalled test completion and drove its own control flow through
 * the Cpm (Converse Poly-Morphic) interface, which reconverse does not
 * provide. Here that plumbing is a plain Converse handler: a test calls
 * megarecon_ack(), which sends a header-only message to PE 0. Tests whose
 * logic itself was written in Cpm (blkinhand, fibobj, fibthr, nodenum,
 * priotest, specmsg, vars, ring) are not ported; neither are future and
 * posixth (Cfuture, Cpthread) nor vecsend (CmiSyncVectorSend), which use
 * APIs reconverse lacks.
 *
 * To add a test:
 *   1. write testname_moduleinit(), run on every PE at startup, that
 *      registers handlers and initializes Cpv variables;
 *   2. write testname_init(), run on PE 0, that starts one round of the
 *      test; the test must call megarecon_ack() numacks times per round
 *      (numacks 0 means once per PE);
 *   3. declare both below and add a row to tests[].
 *
 * Each test is run once, then (if marked reentrant) five rounds at once,
 * then every test at once; finally the controller idles through a
 * self-message countdown so a runaway message would still be caught.
 * Command line: "-name" skips a test; "-only -name ..." runs only those.
 */

#include "megarecon.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

void handler_init(void);
void handler_moduleinit(void);
void ringsimple_init(void);
void ringsimple_moduleinit(void);
void bigmsg_init(void);
void bigmsg_moduleinit(void);
void broadc_init(void);
void broadc_moduleinit(void);
void multicast_init(void);
void multicast_moduleinit(void);
void multisend_init(void);
void multisend_moduleinit(void);
void deadlock_init(void);
void deadlock_moduleinit(void);
void reduction_init(void);
void reduction_moduleinit(void);
void nodereduction_init(void);
void nodereduction_moduleinit(void);

struct testinfo {
  const char *name;
  void (*initiator)(void);
  void (*initializer)(void);
  int reentrant;
  int numacks;
};

static testinfo tests[] = {
    {"handler", handler_init, handler_moduleinit, 1, 1},
    {"ringsimple", ringsimple_init, ringsimple_moduleinit, 0, 10},
    {"bigmsg", bigmsg_init, bigmsg_moduleinit, 1, 1},
    {"broadc", broadc_init, broadc_moduleinit, 1, 1},
    {"multicast", multicast_init, multicast_moduleinit, 1, 1},
    {"multisend", multisend_init, multisend_moduleinit, 0, 1},
    {"deadlock", deadlock_init, deadlock_moduleinit, 0, 2},
    {"reduction", reduction_init, reduction_moduleinit, 0, 1},
    {"nodereduction", nodereduction_init, nodereduction_moduleinit, 0, 1},
    {0, 0, 0, 0, 0},
};

struct ControlMsg {
  CmiMessageHeader header;
  int n;
};

CpvDeclare(int, ack_idx);
CpvDeclare(int, countdown_idx);
CpvDeclare(int, stop_idx);
CpvDeclare(int, test_bank_size);
CpvDeclare(int, test_negate_skip);
CpvDeclare(char **, tests_to_skip);
CpvDeclare(int, num_tests_to_skip);
CpvDeclare(double, test_start_time);
CpvDeclare(int, next_test_index);
CpvDeclare(int, next_test_number);
CpvDeclare(int, acks_expected);
CpvDeclare(int, acks_received);
CpvDeclare(int, finished);

static ControlMsg *control_msg(int handler, int n) {
  ControlMsg *m = (ControlMsg *)CmiAlloc(sizeof(ControlMsg));
  CmiSetHandler(m, handler);
  m->header.messageSize = sizeof(ControlMsg);
  m->n = n;
  return m;
}

void megarecon_ack(void) {
  CmiSyncSendAndFree(0, sizeof(ControlMsg), control_msg(CpvAccess(ack_idx), 0));
}

static void stop_handler(void *vmsg) {
  CmiFree(vmsg);
  CsdExitScheduler();
}

// The shutdown sequence idles for a while, then exits. The idling period
// makes it possible to detect extra runaway messages.
static void countdown_handler(void *vmsg) {
  ControlMsg *m = (ControlMsg *)vmsg;
  if (m->n == 0) {
    CmiFree(m);
    CmiPrintf("exiting.\n");
    CpvAccess(finished) = 1;
    CmiSyncBroadcastAllAndFree(sizeof(ControlMsg),
                               control_msg(CpvAccess(stop_idx), 0));
  } else {
    m->n--;
    CmiSyncSendAndFree(0, sizeof(ControlMsg), m);
  }
}

static int megarecon_skip(const char *test) {
  int num_skip = CpvAccess(num_tests_to_skip);
  char **skip = CpvAccess(tests_to_skip);
  for (int i = 0; i < num_skip; i++)
    if (skip[i][0] == '-' && strcmp(skip[i] + 1, test) == 0)
      return 1 - CpvAccess(test_negate_skip);
  return CpvAccess(test_negate_skip);
}

static void megarecon_next(void) {
  int bank = CpvAccess(test_bank_size);
  int num = CpvAccess(next_test_number);
  for (;;) {
    int idx = CpvAccess(next_test_index);
    if (idx < bank) {
      if (megarecon_skip(tests[idx].name)) {
        CpvAccess(next_test_index)++;
        continue;
      }
      int numacks = tests[idx].numacks;
      CpvAccess(acks_expected) = numacks ? numacks : CmiNumPes();
      CpvAccess(acks_received) = 0;
      CpvAccess(test_start_time) = CmiWallTimer();
      CmiPrintf("test %d: initiated [%s]\n", num, tests[idx].name);
      (tests[idx].initiator)();
      return;
    }
    if (idx < 2 * bank) {
      int pos = idx - bank;
      if (tests[pos].reentrant == 0 || megarecon_skip(tests[pos].name) ||
          CpvAccess(test_negate_skip)) {
        CpvAccess(next_test_index)++;
        continue;
      }
      int numacks = tests[pos].numacks;
      CpvAccess(acks_expected) = 5 * (numacks ? numacks : CmiNumPes());
      CpvAccess(acks_received) = 0;
      CpvAccess(test_start_time) = CmiWallTimer();
      CmiPrintf("test %d: initiated [multi %s]\n", num, tests[pos].name);
      for (int i = 0; i < 5; i++)
        (tests[pos].initiator)();
      return;
    }
    if (idx == 2 * bank) {
      CpvAccess(acks_expected) = 0;
      CpvAccess(acks_received) = 0;
      CpvAccess(test_start_time) = CmiWallTimer();
      CmiPrintf("test %d: initiated [all-at-once]\n", num);
      for (int i = 0; i < bank; i++) {
        if (megarecon_skip(tests[i].name))
          continue;
        int numacks = tests[i].numacks;
        CpvAccess(acks_expected) += numacks ? numacks : CmiNumPes();
        (tests[i].initiator)();
      }
      return;
    }
    if (idx == 2 * bank + 1) {
      CmiPrintf("All tests completed, verifying quiescence...\n");
      CmiSyncSendAndFree(0, sizeof(ControlMsg),
                         control_msg(CpvAccess(countdown_idx), 50000));
      return;
    }
    CmiPrintf("System should have been quiescent, but it wasnt.\n");
    exit(1);
  }
}

static void ack_handler(void *vmsg) {
  CmiFree(vmsg);
  if (CpvAccess(finished)) {
    CmiPrintf("megarecon: ack received after all tests completed.\n");
    exit(1);
  }
  CpvAccess(acks_received)++;
  if (CpvAccess(acks_received) == CpvAccess(acks_expected)) {
    CmiPrintf("test %d: completed (%1.2f sec)\n", CpvAccess(next_test_number),
              CmiWallTimer() - CpvAccess(test_start_time));
    CpvAccess(next_test_number)++;
    CpvAccess(next_test_index)++;
    megarecon_next();
  } else if (CpvAccess(acks_received) > CpvAccess(acks_expected)) {
    CmiPrintf(
        "megarecon: test %d received more acks (%d) than expected (%d).\n",
        CpvAccess(next_test_number), CpvAccess(acks_received),
        CpvAccess(acks_expected));
    exit(1);
  }
}

static void megarecon_init(int argc, char **argv) {
  CpvInitialize(int, ack_idx);
  CpvInitialize(int, countdown_idx);
  CpvInitialize(int, stop_idx);
  CpvInitialize(int, test_bank_size);
  CpvInitialize(int, test_negate_skip);
  CpvInitialize(double, test_start_time);
  CpvInitialize(int, num_tests_to_skip);
  CpvInitialize(char **, tests_to_skip);
  CpvInitialize(int, next_test_index);
  CpvInitialize(int, next_test_number);
  CpvInitialize(int, acks_expected);
  CpvInitialize(int, acks_received);
  CpvInitialize(int, finished);
  CpvAccess(ack_idx) = CmiRegisterHandler(ack_handler);
  CpvAccess(countdown_idx) = CmiRegisterHandler(countdown_handler);
  CpvAccess(stop_idx) = CmiRegisterHandler(stop_handler);
  CpvAccess(finished) = 0;
  for (int i = 0; tests[i].initializer; i++)
    (tests[i].initializer)();

  argc = CmiGetArgc(argv);
  int numtests = 0;
  while (tests[numtests].name)
    numtests++;
  CpvAccess(test_bank_size) = numtests;
  CpvAccess(next_test_index) = 0;
  CpvAccess(next_test_number) = 0;
  CpvAccess(test_negate_skip) = 0;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "-only") == 0)
      CpvAccess(test_negate_skip) = 1;
  CpvAccess(num_tests_to_skip) = argc;
  CpvAccess(tests_to_skip) = argv;
  if (CmiMyPe() == 0)
    megarecon_next();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, megarecon_init);
  return 0;
}
