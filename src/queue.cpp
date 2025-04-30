#include "queue.h"

// Add MutexAccessControl implementation
std::mutex MutexAccessControl::mutex;

void MutexAccessControl::acquire() { mutex.lock(); }

void MutexAccessControl::release() { mutex.unlock(); }
