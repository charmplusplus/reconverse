#include "queue.h"
#include <thread>
#include <vector>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <assert.h>
#include <iostream>

bool testSimplePushPop()
{
    ConverseQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    return q.pop() == 1 && q.pop() == 2 && q.pop() == 3;
}

bool testMultiThreadedPushPop()
{
    ConverseQueue<int> q;
    const int NUM_THREADS = 10;
    const int ITEMS_PER_THREAD = 2000000;

    // Barrier implementation using condition variable
    // std::barrier is not available in C++11
    std::mutex mutex;
    std::condition_variable cv;
    int count = 0;
    bool ready = false;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++)
    {
        threads.push_back(std::thread([&, t]()
                                      {
            {
                std::unique_lock<std::mutex> lock(mutex);
                count++;
                if (count == NUM_THREADS) {
                    ready = true;
                    cv.notify_all();
                } else {
                    cv.wait(lock, [&]{ return ready; });
                }
            }
            
            // Push values
            for (int i = 0; i < ITEMS_PER_THREAD; i++) {
                q.push(t * ITEMS_PER_THREAD + i);
            } }));
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    std::vector<int> values;
    for (int i = 0; i < NUM_THREADS * ITEMS_PER_THREAD; i++)
    {
        values.push_back(q.pop());
    }
    std::sort(values.begin(), values.end());
    for (int i = 0; i < NUM_THREADS * ITEMS_PER_THREAD; i++)
    {
        if (values[i] != i)
        {
            return false;
        }
    }
    return true;
}

int main()
{
    assert(testSimplePushPop());
    assert(testMultiThreadedPushPop());
    std::cout << "All tests have passed" << std::endl; 
    return 0;
}
