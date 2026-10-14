// CCS client side of the test, linked against libccs-client only (no
// Converse). Sends a "ping" to every PE, one broadcast and one multicast,
// checks every expected PE answered, then tells the server to quit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccs-client.h"

static int fail(const char *what, const char *reply) {
  fprintf(stderr, "ccs_client: FAILED: %s\nreply was:\n%s\n", what, reply);
  return 1;
}

// does reply contain "from processor <pe>\n" for this pe?
static int answered(const char *reply, int pe) {
  char needle[64];
  snprintf(needle, sizeof(needle), "from processor %d\n", pe);
  return strstr(reply, needle) != NULL;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: ccs_client host port\n");
    return 2;
  }
  CcsServer svr;
  if (CcsConnect(&svr, argv[1], atoi(argv[2]), NULL) < 0)
    return fail("connect", "");
  int npes = CcsNumPes(&svr);
  printf("ccs_client: connected, %d PEs on %d nodes\n", npes, CcsNumNodes(&svr));
  const char *name = "milind";
  int nameLen = (int)strlen(name) + 1;
  static char reply[65536];
  int len;

  // one request per PE
  for (int pe = 0; pe < npes; ++pe) {
    CcsSendRequest(&svr, "ping", pe, nameLen, name);
    len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
    if (len < 0) return fail("single-PE reply timed out", "");
    reply[len] = 0;
    if (!answered(reply, pe)) return fail("single-PE reply", reply);
  }
  printf("ccs_client: single-PE requests answered by all %d PEs\n", npes);

  // broadcast: the replies of all PEs, concatenated
  CcsSendBroadcastRequest(&svr, "ping", nameLen, name);
  len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
  if (len < 0) return fail("broadcast reply timed out", "");
  reply[len] = 0;
  for (int pe = 0; pe < npes; ++pe)
    if (!answered(reply, pe)) return fail("broadcast reply misses a PE", reply);
  printf("ccs_client: broadcast answered by all %d PEs\n", npes);

  // multicast to the even PEs, listed highest first so pes[0] (the PE that
  // runs the final handler) is not PE 0
  int *pes = (int *)malloc(npes * sizeof(int));
  int n = 0;
  for (int pe = npes - 1; pe >= 0; pe -= 2) pes[n++] = pe;
  // n may exceed 5, so the list tree has more than one level with 2 PEs or more
  CcsSendMulticastRequest(&svr, "ping", n, pes, nameLen, name);
  len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
  if (len < 0) return fail("multicast reply timed out", "");
  reply[len] = 0;
  for (int pe = 0; pe < npes; ++pe) {
    int inList = ((npes - 1 - pe) % 2 == 0);
    if (answered(reply, pe) != inList)
      return fail(inList ? "multicast reply misses a listed PE"
                         : "multicast reply includes an unlisted PE",
                  reply);
  }
  printf("ccs_client: multicast answered by exactly the %d listed PEs\n", n);
  free(pes);

  // The server exits right after answering; the reply may or may not reach
  // us before the socket closes, so no reply is read.
  CcsSendRequest(&svr, "quit", 0, 0, NULL);
  CcsNoResponse(&svr);
  CcsFinalize(&svr);
  printf("ccs_client: PASS\n");
  return 0;
}
