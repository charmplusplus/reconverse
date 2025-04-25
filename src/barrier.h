#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>

// TODO: this is not reusable - on trying to reuse barrier it hangs
class Barrier
{
private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;
    int thread_count;
    int iteration;

public:
    explicit Barrier(int num_threads) : count(0), thread_count(num_threads), iteration(0) {}

    void wait()
    {
        std::unique_lock<std::mutex> lock(mtx);
        int threadlocal_iteration = iteration;
        count++;

        if (count == thread_count)
        {
            count = 0; // Reset barrier for potential reuse
            iteration++;
            cv.notify_all();
            lock.unlock();
        }
        else
        {
            cv.wait(lock, [this, threadlocal_iteration]
                    { return iteration != threadlocal_iteration; });
        }
    }
};
