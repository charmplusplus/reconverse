// Acknowledgement: adopted from
// https://github.com/uiuc-hpc/lci/blob/master/lct/tbarrier/tbarrier.cpp

#include <atomic>
#include <iostream>
#include <thread>

class Barrier {
private:
  alignas(64) std::atomic<int> waiting;
  alignas(64) std::atomic<int64_t> step;
  alignas(64) int thread_num_;

public:
  explicit Barrier(int thread_num)
      : waiting(0), step(0), thread_num_(thread_num) {}

  int64_t arrive() {
    int64_t mstep = step.load();
    if (++waiting == thread_num_) {
      waiting = 0;
      step++;
    }
    return mstep;
  }

  bool test_ticket(int64_t ticket) { return ticket != step; }

  void wait_ticket(int64_t ticket) {
    while (!test_ticket(ticket))
      continue;
  }

  void wait() {
    int64_t ticket = arrive();
    wait_ticket(ticket);
  }
};
