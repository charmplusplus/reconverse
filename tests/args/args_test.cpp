// Command-line parsing: CmiGetArgInt/Long/Double/String/Flag and their
// *Desc forms, CmiGetArgc, CmiReadSize, CmiArgGroup, CmiDeprecateArgInt.
// Every PE parses its own copy of argv (ConverseInit hands each PE one) and
// checks that a parsed argument is removed from argv, that absent arguments
// leave argv alone, and that the values are right. ctest passes:
//   -int 42 -intglued=7 -long 5000000000 -dbl 2.5 -str hello -flag
//   -size 3M -old 9        (14 words)
#include "converse.h"
#include <cstdio>
#include <cstring>

static void check(bool ok, const char *what) {
  if (!ok)
    CmiAbort("args test on PE %d: %s", CmiMyPe(), what);
}

static void mymain(int argc, char **argv) {
  int before = CmiGetArgc(argv);
  check(before >= 14, "test needs its arguments; run it through ctest");

  CmiArgGroup("Converse", "args test"); // prints only under -?

  int i = 0;
  check(CmiGetArgInt(argv, "-int", &i) == 1 && i == 42, "-int 42");
  check(CmiGetArgc(argv) == before - 2, "-int and its value not removed");
  check(CmiGetArgInt(argv, "-int", &i) == 0, "-int found twice");

  int g = 0;
  check(CmiGetArgIntDesc(argv, "-intglued", &g, "glued form") == 1 && g == 7,
        "-intglued=7");

  CmiInt8 l = 0;
  check(CmiGetArgLongDesc(argv, "-long", &l, "a long") == 1 &&
            l == 5000000000LL,
        "-long 5000000000");
  check(CmiGetArgLong(argv, "-long", &l) == 0, "-long found twice");

  double d = 0;
  check(CmiGetArgDouble(argv, "-dbl", &d) == 1 && d == 2.5, "-dbl 2.5");
  check(CmiGetArgDoubleDesc(argv, "-nodbl", &d, "absent") == 0 && d == 2.5,
        "absent double changed the destination");

  char *s = NULL;
  check(CmiGetArgStringDesc(argv, "-str", &s, "a string") == 1 && s &&
            strcmp(s, "hello") == 0,
        "-str hello");
  check(CmiGetArgString(argv, "-str", &s) == 0, "-str found twice");

  check(CmiGetArgFlag(argv, "-flag") == 1, "-flag");
  check(CmiGetArgFlagDesc(argv, "-flag", "again") == 0, "-flag found twice");
  check(CmiGetArgFlag(argv, "-noflag") == 0, "absent flag reported present");

  char *sz = NULL;
  check(CmiGetArgString(argv, "-size", &sz) == 1, "-size 3M");
  check(CmiReadSize(sz) == 3.0 * 1024 * 1024, "CmiReadSize(3M)");
  check(CmiReadSize("2k") == 2048.0, "CmiReadSize(2k)");
  check(CmiReadSize("1G") == 1024.0 * 1024 * 1024, "CmiReadSize(1G)");
  check(CmiReadSize("512") == 512.0, "CmiReadSize(512)");

  CmiDeprecateArgInt(argv, "-old", "an old option",
                     "args test: -old is deprecated (expected warning)");
  check(CmiGetArgInt(argv, "-old", &i) == 0, "-old not consumed");

  check(CmiGetArgc(argv) == before - 14, "wrong number of arguments left");
  check(CmiArgGivingUsage() == 0, "usage mode without -?");
  CmiPrintf("[%d] argument parsing ok, %d arguments consumed\n", CmiMyPe(),
            before - CmiGetArgc(argv));
  CsdExitScheduler();
}

int main(int argc, char **argv) {
  ConverseInit(argc, argv, mymain);
  return 0;
}
