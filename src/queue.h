#ifndef QUEUE_H
#define QUEUE_H

#include <queue>
#include <mutex>
#include <stdexcept>
#include <optional>
#include "concurrentqueue.h"

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

template<typename MessageType>
class AtomicAccessControl {
    // what default size?
    moodycamel::ConcurrentQueue<MessageType> q{8192};

    public:
    void push(MessageType message) {
        q.enqueue(message);
    }

    QueueResult<MessageType> pop_result() {
        MessageType message;
        bool success = q.try_dequeue(message);
        return success ? QueueResult<MessageType>(message) : std::nullopt;
    }

    size_t size() {
        return q.size_approx();
    }

    bool empty() {
        return q.size_approx() == 0;
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