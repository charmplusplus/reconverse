#include "converse.h"
#include <unistd.h>

CmiStartFn mymain(int argc, char** argv)
{
  CmiBarrier();

  char hostname[256];
  gethostname(hostname, sizeof(hostname));

  if (CmiMyPe() == 0) {
    CmiPrintf("CmiNumPhysicalNodes() = %d\n", CmiNumPhysicalNodes());
    CmiPrintf("CmiNumNodes()         = %d\n", CmiNumNodes());
    CmiPrintf("CmiNumPes()           = %d\n", CmiNumPes());
    CmiPrintf("Hostname = %s\n", hostname);

    for (int pe = 0; pe < CmiNumPes(); pe++) {
      CmiPrintf("PE %d: node=%d physicalNode=%d physicalRank=%d\n", pe,
                CmiNodeOf(pe), CmiPhysicalNodeID(pe), CmiPhysicalRank(pe));
    }
  } else {
    CmiPrintf("Hostname = %s\n", hostname);
  }

  CmiBarrier();
  if (CmiMyPe() == 0) CmiExit(0);
  return 0;
}

int main(int argc, char** argv)
{
  ConverseInit(argc, argv, (CmiStartFn)mymain);
  return 0;
}
