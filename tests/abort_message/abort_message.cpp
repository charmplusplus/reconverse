// Checks that CmiAbort prints a reason longer than 255 characters in full:
// the reason is 600 characters and the ctest entry looks for its last chunk.
#include "converse.h"

CmiStartFn mymain(int argc, char **argv) {
  if (CmiMyPe() == 0) {
    char reason[601];
    for (int i = 0; i < 10; i++)
      snprintf(reason + 60 * i, 61, "chunk-%02d-%041d-ends-here", i, 0);
    CmiAbort("%s", reason);
  }
  return 0;
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, (CmiStartFn)mymain, 0, 0);
  return 0;
}
