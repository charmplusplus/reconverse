#ifndef QUEUE_H
#define QUEUE_H

#include <queue>
#include <mutex>
#include <stdexcept>
#include "concurrentqueue.h"

class QueueResult{
    public:
    void *msg;
    operator bool(){
        return msg != NULL;
    }
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

    MessageType pop_throw() {
         std::lock_guard<std::mutex> lock(mtx);
        if (q.empty()) {
             throw std::runtime_error("Cannot pop from empty queue");
        }
        MessageType message = q.front();
        q.pop();
        return message;
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


// An MPSC queue that can be used to send messages between threads.
template <typename MessageType, typename AccessControlPolicy>
class MPSCQueue
{
    AccessControlPolicy policy;

public:
    MessageType pop()
    {
        return policy.pop_throw();
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
using ConverseQueue = MPSCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

template <typename MessageType>
using ConverseNodeQueue = MPMCQueue<MessageType, MutexAccessControl<std::queue<MessageType>, MessageType>>;

#endif
