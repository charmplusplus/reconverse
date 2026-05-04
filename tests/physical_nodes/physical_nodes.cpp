#include "converse.h"

CmiStartFn mymain(int argc, char** argv)
{
  CmiBarrier();

  if (CmiMyPe() == 0)
  {
    CmiPrintf("CmiNumPhysicalNodes() = %d\n", CmiNumPhysicalNodes());
    CmiPrintf("CmiNumNodes()         = %d\n", CmiNumNodes());
    CmiPrintf("CmiNumPes()           = %d\n", CmiNumPes());

    for (int pe = 0; pe < CmiNumPes(); pe++)
    {
      CmiPrintf("PE %d: node=%d physicalNode=%d physicalRank=%d\n", pe,
                CmiNodeOf(pe), CmiPhysicalNodeID(pe), CmiPhysicalRank(pe));
    }
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
