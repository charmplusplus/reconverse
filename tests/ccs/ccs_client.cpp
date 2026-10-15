// CCS client side of the test, linked against libccs-client only (no
// Converse). Sends a "ping" to every PE, one broadcast and one multicast,
// then a delayed-reply "dping" to one PE and as a multicast, checks every
// expected PE answered; then sends malformed requests, which the server must
// turn down without failing, and tells it to quit.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ccs-client.h"
#include "sockRoutines.h"

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

// A request the server turns down gets its socket closed without a reply.
// The default socket error handler exits; this one records the error.
static int lastSocketError;
static int socketError(SOCKET, int code, const char *) {
  lastSocketError = code;
  return -1;
}

// was the request just sent turned down (socket closed, no reply)?
static int rejected(CcsServer *svr, char *reply, int size) {
  lastSocketError = 0;
  return CcsRecvResponse(svr, size, reply, 60) < 0 && lastSocketError == 93620;
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

  // delayed replies (CcsDelayReply), which copy the request header and, for
  // a multicast, the PE list after it: one to the highest PE, then the
  // multicast above again
  CcsSendRequest(&svr, "dping", npes - 1, 0, NULL);
  len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
  if (len < 0) return fail("delayed single-PE reply timed out", "");
  reply[len] = 0;
  if (!answered(reply, npes - 1)) return fail("delayed single-PE reply", reply);
  // the same pes[] again: CcsSendMulticastRequest must not have changed it
  for (int i = 0; i < n; ++i)
    if (pes[i] != npes - 1 - 2 * i)
      return fail("CcsSendMulticastRequest changed the caller's PE list", "");
  CcsSendMulticastRequest(&svr, "dping", n, pes, 0, NULL);
  len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
  if (len < 0) return fail("delayed multicast reply timed out", "");
  reply[len] = 0;
  for (int pe = 0; pe < npes; ++pe) {
    int inList = ((npes - 1 - pe) % 2 == 0);
    if (answered(reply, pe) != inList)
      return fail(inList ? "delayed multicast reply misses a listed PE"
                         : "delayed multicast reply includes an unlisted PE",
                  reply);
  }
  printf("ccs_client: delayed replies answered, single-PE and multicast\n");

  // malformed requests: each is turned down on PE 0, and the server lives on
  skt_abortFn oldAbort = skt_set_abort(socketError);
  for (int pe = 0; pe < npes; ++pe) pes[pe] = pe;
  CcsSendMulticastRequest(&svr, "ping", npes, pes, nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("multicast listing every PE was not turned down", "");
  CcsSendBroadcastRequest(&svr, "nosuchhandler", nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("broadcast to an unknown handler was not turned down", "");
  CcsSendBroadcastRequest(&svr, "nomerge", nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("broadcast to a handler without a merge function was not turned down", "");
  pes[0] = 0; pes[1] = npes;
  CcsSendMulticastRequest(&svr, "ping", 2, pes, nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("multicast to a PE that does not exist was not turned down", "");
  pes[0] = npes - 1; pes[1] = npes - 1;
  CcsSendMulticastRequest(&svr, "ping", 2, pes, nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("multicast listing a PE twice was not turned down", "");
  // a handler field with no terminating NUL (32 characters fill it)
  CcsSendRequest(&svr, "pingpingpingpingpingpingpingping", 0, nameLen, name);
  if (!rejected(&svr, reply, sizeof(reply) - 1))
    return fail("unterminated handler name was not turned down", "");
  skt_set_abort(oldAbort);
  CcsSendRequest(&svr, "ping", npes - 1, nameLen, name);
  len = CcsRecvResponse(&svr, sizeof(reply) - 1, reply, 60);
  if (len < 0) return fail("no reply after the malformed requests", "");
  reply[len] = 0;
  if (!answered(reply, npes - 1))
    return fail("reply after the malformed requests", reply);
  printf("ccs_client: malformed requests turned down, server still answers\n");
  free(pes);

  // The server exits right after answering; the reply may or may not reach
  // us before the socket closes, so no reply is read.
  CcsSendRequest(&svr, "quit", 0, 0, NULL);
  CcsNoResponse(&svr);
  CcsFinalize(&svr);
  printf("ccs_client: PASS\n");
  return 0;
}
