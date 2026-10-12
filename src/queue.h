#ifndef QUEUE_H
#define QUEUE_H

#include <deque>
#include <queue>
#include <mutex>
#include <stdexcept>
#include <optional>
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
// per process (issue #271). The count below makes the empty check one load,
// the way classic Converse's PCQueue keeps a len field: producers add after
// their enqueue is complete (release), the consumer reads it (acquire) and
// only then asks moodycamel, so a positive count means the enqueue is visible.
// A zero count can lag an enqueue by one increment, which only delays the
// message to the next poll. The count is on its own cache line so that the
// producers' RMWs do not share a line with the queue's own state.
template<typename MessageType>
class AtomicAccessControl {
    // what default size?
    moodycamel::ConcurrentQueue<MessageType> q{8192};
    alignas(CMI_CACHE_LINE_SIZE) std::atomic<long> count{0};

    public:
    void push(MessageType message) {
        q.enqueue(message);
        count.fetch_add(1, std::memory_order_release);
    }

    QueueResult<MessageType> pop_result() {
        if (count.load(std::memory_order_acquire) <= 0) return std::nullopt;
        MessageType message;
        bool success = q.try_dequeue(message);
        if (!success) return std::nullopt; // a producer's increment ran ahead
        count.fetch_sub(1, std::memory_order_relaxed);
        return QueueResult<MessageType>(message);
    }

    size_t size() {
        long n = count.load(std::memory_order_acquire);
        return n > 0 ? (size_t)n : 0;
    }

    bool empty() {
        return count.load(std::memory_order_acquire) <= 0;
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
using ConverseQueue = MPSCQueue<MessageType, AtomicAccessControl<MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, AtomicAccessControl<MessageType>>;

#else
template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;
#endif


#endif