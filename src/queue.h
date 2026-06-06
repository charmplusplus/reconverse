#ifndef QUEUE_H
#define QUEUE_H

#include <queue>
#include <mutex>
#include <stdexcept>
#include <optional>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include "concurrentqueue.h"

template<typename T>
using QueueResult = std::optional<T>;

/*
 * Bounded MPSC ring buffer (Vyukov-style).
 *
 * Many producers, single consumer — exactly what the per-PE inbox needs.
 * A profile of the Charm++ pingpong showed moodycamel::ConcurrentQueue ate
 * ~26% of CPU because it's a general MPMC queue paying MPMC tax on a workload
 * that's actually MPSC. This swap cuts per-op cost to ~one CAS for push and
 * one atomic load + one relaxed store for pop.
 *
 * Layout: one cell per slot, each holding a sequence number + value. A
 * sequence number is the "expected position" — producers find a cell whose
 * sequence == enqueue_pos (claim via CAS, write value, bump seq to pos+1),
 * the consumer finds a cell whose sequence == dequeue_pos+1 (take value,
 * bump seq to dequeue_pos+Capacity). Cells are 64-byte-aligned to avoid
 * false sharing between adjacent slots.
 *
 * Bounded means push() can fail (queue full). Capacity is a compile-time
 * power-of-2 — 8192 is generous for any reasonable in-flight Charm++ load.
 * Exceeding it aborts at the caller (CmiPushPE wraps with an assertion).
 */
template<typename T, size_t Capacity = 8192>
class BoundedMPSCRing {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "BoundedMPSCRing Capacity must be a power of two");

    struct alignas(64) Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    static constexpr size_t MASK = Capacity - 1;

    alignas(64) Cell buffer_[Capacity];
    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    // Single-consumer: not atomic. Only the owning PE reads/writes this.
    alignas(64) size_t dequeue_pos_{0};

public:
    BoundedMPSCRing() {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // Returns true on success, false if the ring is full.
    bool push(T value) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & MASK];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Returns true and writes to `out` if a value was popped.
    bool try_pop(T& out) {
        Cell* cell = &buffer_[dequeue_pos_ & MASK];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        intptr_t diff = (intptr_t)seq - (intptr_t)(dequeue_pos_ + 1);
        if (diff == 0) {
            out = cell->data;
            cell->sequence.store(
                dequeue_pos_ + Capacity, std::memory_order_release);
            ++dequeue_pos_;
            return true;
        }
        return false;
    }

    // Fast-empty: a single relaxed load against the cell at the consumer's
    // current position. Cheaper than try_pop when the queue is empty.
    bool empty() const {
        const Cell* cell = &buffer_[dequeue_pos_ & MASK];
        size_t seq = cell->sequence.load(std::memory_order_acquire);
        return seq != (dequeue_pos_ + 1);
    }
};

/*
 * Access policy backed by BoundedMPSCRing<T*>. Only instantiated for pointer
 * MessageType — that's all the Converse layer uses for in-flight messages.
 *
 * Exposes a try_pop_ptr() fast path that returns the popped pointer directly
 * (nullptr means empty) — the scheduler uses this to avoid std::optional
 * construction on every poll (~1.6% of CPU in the old profile).
 */
template<typename MessageType>
class MPSCRingAccessControl {
    static_assert(std::is_pointer<MessageType>::value,
                  "MPSCRingAccessControl requires a pointer MessageType");
    BoundedMPSCRing<MessageType> q;

public:
    void push(MessageType message) {
        // Returning false means the ring is full. The Converse caller has no
        // way to recover, so aborting is the only honest choice. 8192 entries
        // is far above any sane in-flight depth.
        if (!q.push(message)) {
            std::fprintf(stderr,
                "MPSCRingAccessControl: per-PE inbox overflow. "
                "Increase BoundedMPSCRing Capacity in queue.h.\n");
            std::abort();
        }
    }

    QueueResult<MessageType> pop_result() {
        MessageType message;
        return q.try_pop(message) ? QueueResult<MessageType>(message)
                                  : std::nullopt;
    }

    // Pointer-returning fast path. nullptr = empty.
    MessageType try_pop_ptr() {
        MessageType message = nullptr;
        q.try_pop(message);
        return message;
    }

    size_t size() { return 0; /* not tracked; not on hot path */ }
    bool empty() { return q.empty(); }
};

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

template<typename MessageType>
class AtomicAccessControl {
    // what default size?
    moodycamel::ConcurrentQueue<MessageType> q{8192};
    // Atomic size counter for fast-empty short-circuit. Profile of the
    // Charm++ pingpong showed ~19% of CPU went to moodycamel's try_dequeue
    // on the node queue, which is empty 100% of the time in this workload
    // — but moodycamel's MPMC machinery still walks producers and block
    // indices to discover that. With this counter, the scheduler can skip
    // try_dequeue entirely when nothing has been pushed (single relaxed
    // load + branch instead of a producer-list walk).
    //
    // The counter is approximate (relaxed ordering): a producer might see
    // size==0 momentarily after pushing because moodycamel's enqueue
    // happens before the counter increment. That's a benign one-iter
    // delay; the message gets picked up next time around. Crucially, we
    // never lose a message — the counter is incremented strictly AFTER
    // moodycamel sees the push.
    alignas(64) std::atomic<size_t> approx_size_{0};

    public:
    void push(MessageType message) {
        q.enqueue(message);
        approx_size_.fetch_add(1, std::memory_order_relaxed);
    }

    QueueResult<MessageType> pop_result() {
        if (approx_size_.load(std::memory_order_relaxed) == 0)
            return std::nullopt;
        MessageType message;
        bool success = q.try_dequeue(message);
        if (success) approx_size_.fetch_sub(1, std::memory_order_relaxed);
        return success ? QueueResult<MessageType>(message) : std::nullopt;
    }

    // Pointer-return fast path. Only meaningful for pointer MessageType; the
    // nullptr-as-sentinel convention is safe because the Charm++ runtime
    // never enqueues a null message pointer. Lets the scheduler skip
    // std::optional construction on the hot path.
    template<typename M = MessageType>
    typename std::enable_if<std::is_pointer<M>::value, M>::type try_pop_ptr() {
        if (approx_size_.load(std::memory_order_relaxed) == 0) return nullptr;
        M message = nullptr;
        bool success = q.try_dequeue(message);
        if (success) approx_size_.fetch_sub(1, std::memory_order_relaxed);
        return message;
    }

    size_t size() {
        return approx_size_.load(std::memory_order_relaxed);
    }

    bool empty() {
        return approx_size_.load(std::memory_order_relaxed) == 0;
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

    // Pointer-returning fast pop: returns nullptr if empty. Only available
    // when the underlying policy provides it (SFINAE via overload). Used by
    // the scheduler hot path to avoid std::optional construction. Falls back
    // to the optional path on policies that don't implement it.
    template<typename P = AccessControlPolicy>
    auto try_pop_ptr() -> decltype(std::declval<P&>().try_pop_ptr())
    {
        return policy.try_pop_ptr();
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
// Per-PE inbox: many producers (any other PE sending to us), single consumer
// (the owning PE). Use the bounded MPSC ring — strictly cheaper per op than
// moodycamel for this access pattern.
//
// Define RECONVERSE_USE_MOODYCAMEL_PER_PE to fall back to the old behavior
// for A/B comparison.
#ifdef RECONVERSE_USE_MOODYCAMEL_PER_PE
template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, AtomicAccessControl<MessageType>>;
#else
template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, MPSCRingAccessControl<MessageType>>;
#endif

// Node queue stays MPMC (multiple producers, multiple consumers). Keep
// moodycamel here.
template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, AtomicAccessControl<MessageType>>;

#else
template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;
#endif


#endif