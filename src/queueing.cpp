#include "converse.h"
#include "converse_internal.h"

void QueueInit(Queue q)
{
    if (!q) return;
    q->pq_neg = new std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>();
    q->pq_zero = new std::queue<void*>();
    q->pq_pos = new std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>();
}

void QueueDestroy(Queue q)
{
    if (!q) return;
    delete static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
    delete static_cast<std::queue<void*>*>(q->pq_zero);
    delete static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
}

int QueueEmpty(Queue q)
{
    if (!q) return 1;
    auto pq_neg = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
    auto pq_zero = static_cast<std::queue<void*>*>(q->pq_zero);
    auto pq_pos = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
    return pq_neg->empty() && pq_zero->empty() && pq_pos->empty();
}

int QueueSize(Queue q)
{
    if (!q) return 0;
    auto pq_neg = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
    auto pq_zero = static_cast<std::queue<void*>*>(q->pq_zero);
    auto pq_pos = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
    return pq_neg->size() + pq_zero->size() + pq_pos->size();
}

void QueuePush(Queue q, void* message, long long priority)
{
    if (!q) return;
    if (priority < 0) {
        // make MessagePriorityPair and push to pq_neg
        MessagePriorityPair pair(message, priority);
        auto pq_neg = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
        pq_neg->emplace(pair);
    } else if (priority > 0) {
        MessagePriorityPair pair(message, priority);
        auto pq_pos = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
        pq_pos->emplace(pair);
    } else {
        auto pq_zero = static_cast<std::queue<void*>*>(q->pq_zero);
        pq_zero->push(message);
    }
}

void* QueueTop(Queue q)
{
    if (!q) return NULL;
    auto pq_neg = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
    auto pq_zero = static_cast<std::queue<void*>*>(q->pq_zero);
    auto pq_pos = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
    
    if (!pq_neg->empty()) {
        return pq_neg->top().message;
    } else if (!pq_zero->empty()) {
        return pq_zero->front();
    } else if (!pq_pos->empty()) {
        return pq_pos->top().message;
    }
    return NULL; // Queue is empty
}

void QueuePop(Queue q)
{
    if (!q) return;
    auto pq_neg = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_neg);
    auto pq_zero = static_cast<std::queue<void*>*>(q->pq_zero);
    auto pq_pos = static_cast<std::priority_queue<MessagePriorityPair, std::vector<MessagePriorityPair>, MessagePriorityComparator>*>(q->pq_pos);
    
    if (!pq_neg->empty()) {
        pq_neg->pop();
    } else if (!pq_zero->empty()) {
        pq_zero->pop();
    } else if (!pq_pos->empty()) {
        pq_pos->pop();
    }
}

