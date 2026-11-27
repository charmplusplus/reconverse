#ifndef QUEUE_H
#define QUEUE_H

#include <deque>
#include <queue>
#include <mutex>
#include <stdexcept>
#include <optional>
#include <memory>
#include <cstdint>
#include <type_traits>
#include <thread>
#include <vector>
#include <algorithm>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h> // _mm_pause
#elif defined(_M_ARM64)
#include <intrin.h> // __yield
#endif
#include "concurrentqueue.h"
#include <atomic>
#include "converse.h" // CMI_CACHE_LINE_SIZE

template<typename T>
using QueueResult = std::optional<T>;

template<typename ConcreteQ, typename MessageType>
class MutexAccessControl {
    ConcreteQ q;
    std::mutex mtx;

public:
    void push(MessageType message) {
        std::lock_guard<std::mutex> lock(mtx);
        q.push(message);
    }

    QueueResult<MessageType> pop_result() {
        std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) {
            return std::nullopt;
        } else {
            MessageType val = q.front();
            q.pop();
            return QueueResult<MessageType>(val);
        }
    }


    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return q.size();
    }

    bool empty() {
         std::lock_guard<std::mutex> lock(mtx);
        return q.empty();
    }
};

// moodycamel keeps one sub-queue per producer thread, and its size_approx()
// and try_dequeue() walk every sub-queue ever created, so on their own an
// empty check costs O(producers): about 16 ns per producer, 1 us at 64 PEs
// per process (issue #271). The counters below make the empty check one load
// at most, the way classic Converse's PCQueue keeps a len field. A pop that
// finds a message still walks the sub-queues, which ThreadQueue below avoids;
// this class serves the node queue and +thread_queue_per_producer.
//
// Producers add to `pushed` after their enqueue is complete (release). The
// consumer never writes that line: it keeps its own `taken` count and, with
// a single consumer, a private `credit` of messages it has learned about,
// so a stream of messages costs the consumer one acquire load of `pushed`
// per batch rather than one per message, and the producers' line is never
// pulled away by a consumer write. A positive credit means the enqueues are
// visible, since the load that produced it acquired the producers' releases.
// A zero credit can lag an enqueue by one increment, which only delays the
// message to the next poll. With several consumers (the node queue) `taken`
// is shared and there is no credit, so every pop reloads `pushed`.
template<typename MessageType, bool kSingleConsumer = true>
class AtomicAccessControl {
    // what default size?
    moodycamel::ConcurrentQueue<MessageType> q{8192};
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<long> pushed{0};   // producers' line
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<long> taken{0};    // consumers' line
    long credit = 0; // single consumer only: known pushed, not yet taken

    long available() {
        if (kSingleConsumer && credit > 0) return credit;
        long n = pushed.load(std::memory_order_acquire) -
                 taken.load(std::memory_order_relaxed);
        if (kSingleConsumer) credit = n > 0 ? n : 0;
        return n;
    }

    public:
    void push(MessageType message) {
        q.enqueue(message);
        pushed.fetch_add(1, std::memory_order_release);
    }

    QueueResult<MessageType> pop_result() {
        if (available() <= 0) return std::nullopt;
        MessageType message;
        if (!q.try_dequeue(message)) {
            // a producer's increment ran ahead of its enqueue (or another
            // consumer took it): forget the credit and look again next time
            if (kSingleConsumer) credit = 0;
            return std::nullopt;
        }
        if (kSingleConsumer) { credit--; taken.store(taken.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
        else taken.fetch_add(1, std::memory_order_relaxed);
        return QueueResult<MessageType>(message);
    }

    size_t size() {
        long n = available();
        return n > 0 ? (size_t)n : 0;
    }

    bool empty() {
        return available() <= 0;
    }
};

// For a wait the thread queue below makes rare, a producer at a segment
// boundary waiting for the one that links the next segment: pause in the
// spin, which hands the core to a sibling hardware thread, and after a while
// yield it, so that the thread the wait depends on can run when there are
// more threads than cores.
class QueueBackoff {
    unsigned spins = 0;
public:
    void wait() {
        if (++spins < 64) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
            _mm_pause();
#elif defined(_M_ARM64)
            __yield();
#elif defined(__aarch64__) || defined(__arm__)
            asm volatile("yield" ::: "memory");
#endif
        } else {
            std::this_thread::yield();
        }
    }
};

// The PE thread queue (issue #271): one FIFO that every producer shares, so
// taking a message costs the consumer the same with 2 producers or 127.
// AtomicAccessControl above walks its per-producer sub-queues to find a
// message; it stays available with +thread_queue_per_producer.
//
// A chain of segments of kSegSize slots, each slot null or a message. A
// segment is filled once, front to back; when the consumer has drained it, it
// goes back to this queue's pool, to be linked again later.
//
// Push: a producer takes a ticket with one fetch_add on the tail segment's
// counter and stores its message in that ticket's slot. Every slot is null
// when its segment is linked, and each ticket is handed out once per linking,
// so a producer never waits for the consumer (a PE pushes to its own queue),
// and any number of threads may push.
//
// Pop: the consumer takes the slots in order and nulls each one it takes, so
// its empty check and its pop each load the one slot at its head. Slot
// kSegSize is never filled. The consumer stops there and moves on once the
// next segment is linked, returning the drained one to the pool.
//
// The boundary: ticket kSegSize goes to exactly one producer per linking.
// That producer takes a segment from the pool and, in this order, makes it
// tailSeg, links it after the full one, and opens it by setting its counter
// to 0. A producer whose ticket is past kSegSize waits until tailSeg moves on
// or the segment it tried opens again, and then starts over from tailSeg. It
// may have tried while that segment sat in the pool, and the segment may
// since have become tailSeg again and even filled; the wait ends whenever no
// one holds that segment's ticket kSegSize, so that the producer can take it.
//
// Segments come back, so a producer may take a ticket on a segment it read in
// an earlier life of that segment. Three rules make that harmless:
//   1. A segment in the pool, or claimed and not yet open, carries kClosed in
//      its counter, so a ticket on it is refused. A ticket taken after the
//      segment opens again names a slot of this same queue, which delivers
//      it with the rest of that life.
//   2. Only the producer holding ticket kSegSize moves tailSeg on. Otherwise
//      a delayed producer could move it to, or past, a segment in that
//      segment's next life, and the consumer would wait forever at slots that
//      no ticket fills.
//   3. tailSeg moves before the link is stored. The consumer recycles a
//      segment only after reading its link, so tailSeg has left a segment
//      before the segment can come back, and tailSeg only moves along the
//      chain.
// Segments are freed only with the queue, because a delayed producer may
// still touch one.
//
// The pool is one cache line of segment pointers. A producer claims an entry
// by exchanging it with null, which is the whole claim (no ABA). Only the
// consumer fills entries, so a plain store into one it reads as null is
// enough; when all are full it keeps the rest on a list of its own and moves
// them in as entries empty. The queue starts with two segments, both touched
// by the constructing thread (the consumer), one linked and one in the pool.
// With a small backlog it alternates between those two. After a burst it
// keeps the segments the burst needed (about 8 KB each) and reuses them.
//
// Order is ticket order: FIFO per producer, and across producers in the order
// their tickets were taken. Only the owning PE may call pop, empty and size.
template <typename MessageType>
class ThreadQueue {
    static_assert(std::is_pointer<MessageType>::value,
                  "ThreadQueue holds pointers: null marks an empty slot");
    static constexpr uint64_t kSegSize = 1024;
    static constexpr uint64_t kClosed = 1ull << 63;
    static constexpr int kPoolSlots = CMI_CACHE_LINE_SIZE / sizeof(void *);

    struct Segment {
        alignas(CMI_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{kClosed}; // producers' line
        alignas(CMI_CACHE_LINE_SIZE) std::atomic<Segment *> next{nullptr};
        Segment *allNext = nullptr; // every segment, for the destructor
        alignas(CMI_CACHE_LINE_SIZE) std::atomic<MessageType> slots[kSegSize + 1];

        Segment() {
            for (auto &s : slots) s.store(nullptr, std::memory_order_relaxed);
        }
    };

    alignas(CMI_CACHE_LINE_SIZE) std::atomic<Segment *> tailSeg; // producers start here
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<Segment *> pool[kPoolSlots];
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<Segment *> allSegs{nullptr};
    // consumer only
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<MessageType> *headSlot;
    std::atomic<MessageType> *headEnd; // &headSeg->slots[kSegSize], never filled
    Segment *headSeg = nullptr;
    std::vector<Segment *> spare; // drained segments the pool had no entry for
    std::unique_ptr<AtomicAccessControl<MessageType>> perProducer;

    Segment *allocate() {
        Segment *s = new Segment;
        Segment *h = allSegs.load(std::memory_order_relaxed);
        do {
            s->allNext = h;
        } while (!allSegs.compare_exchange_weak(h, s, std::memory_order_release,
                                                std::memory_order_relaxed));
        return s;
    }

    // A closed segment for the producer that links the next one.
    Segment *claim() {
        for (auto &p : pool) {
            if (p.load(std::memory_order_relaxed) == nullptr) continue;
            Segment *s = p.exchange(nullptr, std::memory_order_acq_rel);
            if (s != nullptr) return s;
        }
        return allocate();
    }

    // The consumer is done with s: close it and put it back.
    void recycle(Segment *s) {
        s->tail.store(kClosed, std::memory_order_relaxed);
        s->next.store(nullptr, std::memory_order_relaxed);
        spare.push_back(s);
        for (auto &p : pool) {
            if (spare.empty()) break;
            if (p.load(std::memory_order_relaxed) != nullptr) continue;
            p.store(spare.back(), std::memory_order_release);
            spare.pop_back();
        }
    }

    // At headEnd: move to the next segment if one is linked.
    bool advance() {
        Segment *n = headSeg->next.load(std::memory_order_acquire);
        if (n == nullptr) return false;
        Segment *done = headSeg;
        headSeg = n;
        headSlot = &n->slots[0];
        headEnd = &n->slots[kSegSize];
        recycle(done);
        return true;
    }

    MessageType take() {
        for (;;) {
            MessageType m = headSlot->load(std::memory_order_acquire);
            if (m != nullptr) {
                headSlot->store(nullptr, std::memory_order_relaxed);
                ++headSlot;
                return m;
            }
            if (headSlot != headEnd || !advance()) return nullptr;
        }
    }

public:
    explicit ThreadQueue(bool perProducerQueue = false) {
        for (auto &p : pool) p.store(nullptr, std::memory_order_relaxed);
        if (perProducerQueue) {
            perProducer.reset(new AtomicAccessControl<MessageType>());
            tailSeg.store(nullptr, std::memory_order_relaxed);
            headSlot = headEnd = nullptr;
            return;
        }
        headSeg = allocate();
        headSeg->tail.store(0, std::memory_order_relaxed);
        headSlot = &headSeg->slots[0];
        headEnd = &headSeg->slots[kSegSize];
        tailSeg.store(headSeg, std::memory_order_relaxed);
        pool[0].store(allocate(), std::memory_order_relaxed);
    }
    ThreadQueue(const ThreadQueue &) = delete;
    ThreadQueue &operator=(const ThreadQueue &) = delete;
    ~ThreadQueue() {
        for (Segment *s = allSegs.load(std::memory_order_relaxed); s != nullptr;) {
            Segment *n = s->allNext;
            delete s;
            s = n;
        }
    }

    void push(MessageType message) {
        if (message == nullptr) CmiAbort("ThreadQueue: null message pushed");
        if (perProducer) { perProducer->push(message); return; }
        QueueBackoff backoff;
        for (;;) {
            Segment *s = tailSeg.load(std::memory_order_acquire);
            uint64_t t = s->tail.fetch_add(1, std::memory_order_acq_rel);
            if (t < kSegSize) {
                s->slots[t].store(message, std::memory_order_release);
                return;
            }
            if (t == kSegSize) {
                // s is full and this producer links the next segment, which
                // stays closed until it is linked (rules 2 and 3)
                Segment *n = claim();
                tailSeg.store(n, std::memory_order_release);
                s->next.store(n, std::memory_order_release);
                n->tail.store(0, std::memory_order_release);
                continue;
            }
            // s is full or closed. Wait while tailSeg is s and s's counter is
            // past kSegSize: either a producer holds ticket kSegSize and will
            // move tailSeg on, or s is closed and its claimer will open it.
            // A counter at kSegSize or below means s has opened again since
            // this producer's ticket, so it tries again.
            while (tailSeg.load(std::memory_order_acquire) == s &&
                   s->tail.load(std::memory_order_relaxed) > kSegSize)
                backoff.wait();
        }
    }

    QueueResult<MessageType> pop() {
        if (perProducer) return perProducer->pop_result();
        MessageType m = take();
        if (m == nullptr) return std::nullopt;
        return QueueResult<MessageType>(m);
    }

    bool empty() {
        if (perProducer) return perProducer->empty();
        for (;;) {
            if (headSlot->load(std::memory_order_acquire) != nullptr) return false;
            if (headSlot != headEnd || !advance()) return true;
        }
    }

    // An estimate, for the randomized scheduler's drain: tickets handed out
    // and not yet taken, a ticket whose message is still being stored
    // included.
    size_t size() {
        if (perProducer) return perProducer->size();
        size_t n = 0;
        uint64_t from = headSlot - &headSeg->slots[0];
        for (Segment *s = headSeg; s != nullptr; s = s->next.load(std::memory_order_acquire)) {
            uint64_t t = s->tail.load(std::memory_order_acquire);
            if (t & kClosed) break; // linked, not yet opened
            n += std::min(t, kSegSize) - from;
            from = 0;
        }
        return n;
    }
};

// An MPSC queue that can be used to send messages between threads.
template <typename MessageType, typename AccessControlPolicy>
class MPSCQueue
{
    AccessControlPolicy policy;

public:
    QueueResult<MessageType> pop()
    {
        return policy.pop_result();
    }

    void push(MessageType message)
    {
        policy.push(message);
    }

    bool empty()
    {
        return policy.empty();
    }

    size_t size()
    {
        return policy.size();
    }
};

template <typename MessageType, typename AccessControlPolicy>
class MPMCQueue
{
    AccessControlPolicy policy;

public:
    QueueResult<MessageType> pop()
    {
        return policy.pop_result();
    }

    void push(MessageType message)
    {
        policy.push(message);
    }

    bool empty()
    {
        return policy.empty();
    }

    size_t size()
    {
        return policy.size();
    }
};

#ifdef ATOMIC_QUEUE_ENABLED
template <typename MessageType>
using ConverseQueue = ThreadQueue<MessageType>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, AtomicAccessControl<MessageType, false>>;

#else
template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;
#endif


#endif