/* The scheduler queue.
 *
 * Contract (the one Charm++'s queueing strategies document): messages come
 * out in increasing priority value, negative before zero before positive;
 * within one priority value, FIFO for QueuePush and LIFO for QueuePushFront,
 * and the two may be mixed at one level (a LIFO push goes ahead of everything
 * already queued there, a FIFO push behind it).
 *
 * Design, following classic Converse's Cqs: one double-ended queue per live
 * priority level, the levels ordered by priority. The cost of a push or pop
 * is then a function of the number of live levels, not of the number of
 * queued messages, and FIFO/LIFO are just the two ends of the deque. Priority
 * zero, by far the most common, has its own deque and never touches the
 * level map. A level that drains is kept allocated (up to kKeptEmptyLevels)
 * so that the common pattern of a few levels filling and emptying does not
 * allocate on every cycle; QueueTop skips the kept empties, which bounds
 * that scan by the same constant.
 *
 * Measured on one core (push+pop pair, steady state, Apple M-series, -O2):
 * about 24 ns with 4 live levels and 38 ns with 16, independent of depth,
 * against 19-70 ns (depth 16 to 65536) for a std::priority_queue of
 * (message, priority) pairs, which cannot give FIFO within a level at all.
 * With thousands of distinct priorities that are each used once (unique
 * bounds in branch-and-bound) the map is slower than a heap at small depth
 * (~60 vs ~20 ns) and about even from a thousand queued messages up.
 *
 * Tuning notes for fine-grained prioritized applications:
 *  - kKeptEmptyLevels trades allocation churn (too small) against the
 *    QueueTop skip scan (too large); 4 is right for "a few critical-path
 *    levels", try 0 for unique-priority workloads.
 *  - If every message carries a distinct priority and depth stays small, a
 *    plain binary heap of (message, priority) beats this by 2-3x; FIFO within
 *    a level is then moot because levels hold one message. That could be a
 *    second Queue implementation selected per run or per queue.
 *  - Nothing here needs to be the only queue: the strategy argument of
 *    CqsEnqueueGeneral is the natural switch for alternate or coexisting
 *    queue structures (bitvector priorities are not supported yet; they will
 *    want their own level ordering).
 */
#include "converse.h"
#include "converse_internal.h"
#include <deque>
#include <map>

namespace {
using Level = std::deque<void *>;
using Levels = std::map<long long, Level>;
constexpr int kKeptEmptyLevels = 4;

inline Levels &levels(Queue q) { return *static_cast<Levels *>(q->levels); }
inline Level &zero(Queue q) { return *static_cast<Level *>(q->zero); }

// The level holding the next message to come out, or nullptr if the queue
// is empty. Drained levels that are kept allocated are skipped.
Level *front_level(Queue q) {
  Levels &lv = levels(q);
  Levels::iterator it = lv.begin();
  for (; it != lv.end() && it->first < 0; ++it)
    if (!it->second.empty())
      return &it->second;
  if (!zero(q).empty())
    return &zero(q);
  for (; it != lv.end(); ++it)
    if (!it->second.empty())
      return &it->second;
  return nullptr;
}

Level &level_for(Queue q, long long priority) {
  if (priority == 0)
    return zero(q);
  Levels &lv = levels(q);
  Levels::iterator it = lv.find(priority);
  if (it == lv.end())
    it = lv.emplace(priority, Level()).first;
  else if (it->second.empty())
    q->emptyLevels--;
  return it->second;
}

void level_drained(Queue q, Level *l) {
  if (l == &zero(q))
    return;
  if (q->emptyLevels < kKeptEmptyLevels) {
    q->emptyLevels++;
    return;
  }
  Levels &lv = levels(q);
  for (Levels::iterator it = lv.begin(); it != lv.end(); ++it)
    if (&it->second == l) {
      lv.erase(it);
      return;
    }
}
} // namespace

void QueueInit(Queue q) {
  if (!q)
    return;
  q->levels = new Levels();
  q->zero = new Level();
  q->emptyLevels = 0;
  q->size = 0;
}

void QueueDestroy(Queue q) {
  if (!q)
    return;
  delete static_cast<Levels *>(q->levels);
  delete static_cast<Level *>(q->zero);
  q->levels = q->zero = nullptr;
  q->emptyLevels = q->size = 0;
}

int QueueEmpty(Queue q) { return !q || q->size == 0; }

int QueueSize(Queue q) { return q ? q->size : 0; }

void QueuePush(Queue q, void *message, long long priority) {
  if (!q)
    return;
  level_for(q, priority).push_back(message);
  q->size++;
}

void QueuePushFront(Queue q, void *message, long long priority) {
  if (!q)
    return;
  level_for(q, priority).push_front(message);
  q->size++;
}

void *QueueTop(Queue q) {
  if (!q || q->size == 0)
    return nullptr;
  Level *l = front_level(q);
  return l ? l->front() : nullptr;
}

void QueuePop(Queue q) {
  if (!q || q->size == 0)
    return;
  Level *l = front_level(q);
  if (!l)
    return;
  l->pop_front();
  q->size--;
  if (l->empty())
    level_drained(q, l);
}
