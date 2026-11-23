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

// The PE thread queue (issue #271): one FIFO that every producer shares, so
// taking a message costs the consumer the same with 2 producers or 127.
// AtomicAccessControl above walks its per-producer sub-queues to find a
// message; it stays available with +thread_queue_per_producer.
//
// A ring of slots, each null or a message. A producer takes a ticket with one
// fetch_add on the ring's tail and stores its message in slot ticket % cap.
// The consumer takes the slots in ticket order and nulls each one it takes,
// so its empty check and its pop each load the one slot at its head. It
// publishes its position (headPub) every kPublish messages.
//
// A ticket that lands cap - 2 * kSpare or more past headPub closes the ring
// once its message is stored. With at most kSpare threads pushing at once,
// every ticket handed out before the close names a slot that is already
// null: the ones that passed that check by construction, and the ones after
// the last of those because they come at most one per pushing thread (a
// thread holding one closes the ring before it pushes again). A producer
// still waits for its slot to read null, so a broken bound costs a wait,
// never a message.
//
// The close is a bit in the tail: a producer whose ticket carries it, a
// delayed one included, goes on to the next ring. The closer links that ring,
// twice as large, and then stores a marker in the slot of the first ticket
// not handed out, so the consumer moves on when it reaches the marker. A
// closed ring is freed only with the queue, because a delayed producer may
// still read it; the closed rings add up to less than the live one.
//
// Order is ticket order: FIFO per producer, and across producers in the order
// their tickets were taken. Only the owning PE may call pop, empty and size.
template <typename MessageType>
class ThreadQueue {
    static_assert(std::is_pointer<MessageType>::value,
                  "ThreadQueue holds pointers: null marks an empty slot");
    static constexpr uint64_t kInitialCap = 4096;
    static constexpr uint64_t kSpare = 512;   // threads that may push at once
    static constexpr uint64_t kPublish = 256; // pops between headPub stores
    static constexpr uint64_t kClosed = 1ull << 63;
    static constexpr uint64_t kOpen = ~0ull;  // closedAt until the close
    static_assert((kInitialCap & (kInitialCap - 1)) == 0 &&
                  kInitialCap > 2 * kSpare + kPublish, "ThreadQueue sizes");

    struct Ring {
        alignas(CMI_CACHE_LINE_SIZE) std::atomic<uint64_t> tail{0};    // producers' line
        alignas(CMI_CACHE_LINE_SIZE) std::atomic<uint64_t> headPub{0}; // consumer's line
        // read-mostly from here on
        alignas(CMI_CACHE_LINE_SIZE) const uint64_t cap;
        std::atomic<MessageType> *const slots;
        std::atomic<Ring *> next{nullptr};
        std::atomic<uint64_t> closedAt{kOpen}; // tickets handed out before the close

        explicit Ring(uint64_t c) : cap(c), slots(new std::atomic<MessageType>[c]) {
            for (uint64_t i = 0; i < c; i++)
                slots[i].store(nullptr, std::memory_order_relaxed);
        }
        ~Ring() { delete[] slots; }

        std::atomic<MessageType> &slot(uint64_t ticket) { return slots[ticket & (cap - 1)]; }
    };

    alignas(CMI_CACHE_LINE_SIZE) std::atomic<Ring *> tailRing; // producers start here
    // consumer only
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<MessageType> *headSlot; // the slot of ticket head
    uint64_t head = 0;
    Ring *headRing;
    Ring *const firstRing; // the destructor frees the chain from here
    std::unique_ptr<AtomicAccessControl<MessageType>> perProducer;

    static MessageType closedMark() { return reinterpret_cast<MessageType>(uintptr_t(1)); }

    // The ring after r, linking one if no one has.
    static Ring *link(Ring *r) {
        Ring *n = r->next.load(std::memory_order_acquire);
        if (n != nullptr) return n;
        Ring *fresh = new Ring(r->cap * 2);
        if (r->next.compare_exchange_strong(n, fresh, std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            return fresh;
        delete fresh;
        return n;
    }

    // Closes r unless someone has: links the next ring, then marks the slot of
    // the first ticket not handed out.
    void close(Ring *r) {
        uint64_t end = r->tail.fetch_or(kClosed, std::memory_order_acq_rel);
        if (end & kClosed) return;
        r->closedAt.store(end, std::memory_order_release);
        Ring *n = link(r);
        std::atomic<MessageType> &mark = r->slot(end);
        while (mark.load(std::memory_order_acquire) != nullptr) {} // only past the kSpare bound
        mark.store(closedMark(), std::memory_order_release);
        tailRing.compare_exchange_strong(r, n, std::memory_order_acq_rel,
                                         std::memory_order_relaxed);
    }

    // The marker is at head: every ticket of this ring has been taken, and
    // the next ring was linked before the marker was stored.
    void nextRing() {
        headRing = headRing->next.load(std::memory_order_acquire);
        head = 0;
        headSlot = &headRing->slot(0);
    }

    MessageType take() {
        for (;;) {
            MessageType m = headSlot->load(std::memory_order_acquire);
            if (m == nullptr) return nullptr;
            if (m == closedMark()) { nextRing(); continue; }
            headSlot->store(nullptr, std::memory_order_relaxed);
            if ((++head & (kPublish - 1)) == 0)
                headRing->headPub.store(head, std::memory_order_release);
            headSlot = &headRing->slot(head);
            return m;
        }
    }

public:
    explicit ThreadQueue(bool perProducerQueue = false)
        : firstRing(perProducerQueue ? nullptr : new Ring(kInitialCap)) {
        tailRing.store(firstRing, std::memory_order_relaxed);
        headRing = firstRing;
        headSlot = firstRing ? &firstRing->slot(0) : nullptr;
        if (perProducerQueue) perProducer.reset(new AtomicAccessControl<MessageType>());
    }
    ThreadQueue(const ThreadQueue &) = delete;
    ThreadQueue &operator=(const ThreadQueue &) = delete;
    ~ThreadQueue() {
        for (Ring *r = firstRing; r != nullptr;) {
            Ring *n = r->next.load(std::memory_order_relaxed);
            delete r;
            r = n;
        }
    }

    void push(MessageType message) {
        if (reinterpret_cast<uintptr_t>(message) <= 1) // null, or the marker
            CmiAbort("ThreadQueue: null message pushed");
        if (perProducer) { perProducer->push(message); return; }
        Ring *start = tailRing.load(std::memory_order_acquire);
        Ring *r = start;
        uint64_t t;
        while ((t = r->tail.fetch_add(1, std::memory_order_acq_rel)) & kClosed)
            r = link(r);
        uint64_t h = r->headPub.load(std::memory_order_acquire);
        std::atomic<MessageType> &slot = r->slot(t);
        while (slot.load(std::memory_order_acquire) != nullptr) {} // only past the kSpare bound
        slot.store(message, std::memory_order_release);
        // move the producers' starting ring on; it only ever moves forward,
        // from a ring to one linked after it
        if (r != start)
            tailRing.compare_exchange_strong(start, r, std::memory_order_acq_rel,
                                             std::memory_order_relaxed);
        if (t - h >= r->cap - 2 * kSpare) close(r);
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
            MessageType m = headSlot->load(std::memory_order_acquire);
            if (m != closedMark()) return m == nullptr;
            nextRing();
        }
    }

    // Tickets handed out and not yet taken; a ticket whose message is still
    // being stored counts.
    size_t size() {
        if (perProducer) return perProducer->size();
        size_t n = 0;
        uint64_t from = head;
        for (Ring *r = headRing; r != nullptr; r = r->next.load(std::memory_order_acquire)) {
            uint64_t end = r->closedAt.load(std::memory_order_acquire);
            if (end == kOpen) end = r->tail.load(std::memory_order_acquire) & ~kClosed;
            n += end - from;
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