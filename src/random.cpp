#include "converse_internal.h"
#include <random>
#include <thread>

using Distribution = std::uniform_real_distribution<double>;
using Generator = std::minstd_rand;

thread_local Generator *_defaultStream;
thread_local Distribution *distribution;

void CrnInit(void) {

  distribution = new Distribution(0.0, 1.0);
  _defaultStream =
      new Generator(0); // This should probably be seeded with random_device
}

void CrnSrand(unsigned int seed) { _defaultStream->seed(seed); }

int CrnRand(void) { return (int)(CrnDrand() * 0x80000000U); }

int CrnRandRange(int min, int max) {
  return std::uniform_int_distribution<int>(min, max)(*_defaultStream);
}

double CrnDrand(void) { return (*distribution)(*_defaultStream); }

double CrnDrandRange(double min, double max) {
  return std::uniform_real_distribution<double>(min, max)(*_defaultStream);
}
