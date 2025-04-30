/**
 * this header file defines functinos/declarations/definitions for the task queue system that belongs to converse
 * specifically the work-stealing queue of Cilk (THE protocol).
 * For more details: visit this link from the readthedocs from the old converse documentation: https://charm.readthedocs.io/en/latest/charm++/manual.html#stealable-tasks-for-within-node-load-balancing
 * ALSO we will not be supporitng windows support currently, maybe print out an error message if someone tries to compile on windows? 
 */
#ifndef _CKTASKQUEUE_H
#define _CKTASKQUEUE_H

#include <stdint.h>
#include <stdlib.h>
#include <iostream>

#define TASKQUEUE_SIZE 1024

#if CMK_SMP
    #define CmiMemoryWriteFence() __sync_synchronize() // This is a memory fence to ensure that writes are visible to other threads/cores
#else 
    #define CmiMemoryWriteFence() // No-op if not in SMP mode
#endif


typedef int taskq_idx;

typedef struct TaskQueueStruct {
    taskq_idx head; // This pointer indicates the first task in the queue
    taskq_idx tail; // The tail indicates the array element next to the last available task in the queue. So, if head == tail, the queue is empty
    void *data[TASKQUEUE_SIZE];
} TaskQueue;

// Function to create a new TaskQueue and initialize its members
inline static TaskQueue* TaskQueueCreate() {
   TaskQueue* taskqueue = (TaskQueue*)malloc(sizeof(TaskQueue));
   taskqueue->head = 0; 
   taskqueue->tail = 0; 
   for (int i = 0; i < TASKQUEUE_SIZE; i++) {
       taskqueue->data[i] = NULL;
   }
   return taskqueue; 
}

// Function to push a task onto the TaskQueue
inline static void TaskQueuePush(TaskQueue* queue, void* data) {
    queue->data[queue->tail % TASKQUEUE_SIZE] = data; 
    CmiMemoryWriteFence(); //makes sure the data is fully written before updating the tail pointer
    queue->tail++; 
    if (queue->tail >= TASKQUEUE_SIZE) {
        fprintf(stderr, "TaskQueue overflow: possible corruption/overwrite possibility of data\n");
    }
}

// Function to pop a task from the TaskQueue. Victims pop from the tail 
inline static void* TaskQueuePop(TaskQueue* queue) {
    queue->tail = queue->tail - 1; 
    CmiMemoryWriteFence();
    taskq_idx head = queue->head;
    taskq_idx tail = queue->tail; 
    if (tail > head) { // there are more than two tasks in the queue, so it is safe to pop a task from the queue.
        return queue->data[tail % TASKQUEUE_SIZE];
    }

    if (tail < head) { // The taskqueue is empty and the last task has been stolen by a thief.
        queue->tail = head; // reset the tail pointer to the head pointer
        return NULL;
    }

    // head==tail case: there is only one task so thieves and victim can try to obtain this task simultaneously.
    queue->tail = head + 1;
    if (!__sync_bool_compare_and_swap(&(queue->head), head, head+1)) { // Check whether the last task has already stolen.
        return NULL;
    }
    return queue->data[tail % TASKQUEUE_SIZE];
}

// Function to steal a task from another TaskQueue. Other PEs/Threads steal from the head
inline static void* TaskQueueSteal(TaskQueue* queue) {
    taskq_idx head, tail; 
    while (1) {
        head = queue->head;
        tail = queue->tail;
        if (head >= tail) { 
            // The queue is empty
            // or the last element has been stolen by other thieves 
            // or popped by the victim.
            return NULL;
        }

        if (!__sync_bool_compare_and_swap(&(queue->head), head, head+1)) { // Check whether the task this thief is trying to steal is still in the queue and not stolen by the other thieves.
            continue;
        } 
        return queue->data[head % TASKQUEUE_SIZE];
    }
}

// Function to destroy the TaskQueue and free its memory
inline static void TaskQueueDestroy(TaskQueue* queue) {
    if (queue != NULL) {
        free(queue);
    }
}


#endif