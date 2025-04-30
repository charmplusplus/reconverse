#ifndef QUEUE_H
#define QUEUE_H

#include <mutex>
#include <queue>
#include <stdexcept>
#include "concurrentqueue.h"

class QueueResult {
public:
  void *msg;
  operator bool() { return msg != NULL; }
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

    QueueResult pop_result() {
        std::lock_guard<std::mutex> lock(mtx);
        QueueResult result;
        if (q.empty()) {
            result.msg = NULL;
        } else {
            result.msg = q.front();
            q.pop();
        }
        return result;
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
    moodycamel::ConcurrentQueue<MessageType> q{256};

    public:
    void push(MessageType message) {
        q.enqueue(message);
    }

    QueueResult pop_result() {
        MessageType message;
        bool success = q.try_dequeue(message);
        QueueResult result;
        result.msg = success ? message : nullptr;
        return result;
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
    MessageType pop()
    {
        return policy.pop_result().msg;
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
  QueueResult pop() {
    AccessControlPolicy::acquire();
    // This will not work for atomics.
    // It's fine for now: internal implementation detail.


    QueueResult pop()
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

template <typename MessageType>
using ConverseQueue =
    MPSCQueue<std::queue<MessageType>, MessageType, MutexAccessControl>;

template <typename MessageType>
using ConverseQueue = MPSCQueue<MessageType, AtomicAccessControl<MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, AtomicAccessControl<MessageType>>;

// template <typename MessageType>
// using ConverseQueue = MPSCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

// template <typename MessageType>
// using ConverseNodeQueue = MPMCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;


#endif
