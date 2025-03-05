#include <iostream> 
#include <assert.h>
#include "../barrier.h"
#include <thread>
#include <vector>
#include <future>
#include <chrono>

#define NUM_THREADS 5 

static std::mutex mtx; 
static Barrier barrier(NUM_THREADS);
static std::vector<std::thread> threads;

static long int count = 0; 
static int pass1 = 1;
static int pass2 = 1;

void thread_func_throwaway(int var) {
    std::this_thread::sleep_for(std::chrono::seconds(var)); //simulate work 

    mtx.lock();
    count++;
    mtx.unlock();

    barrier.wait();

    mtx.lock();
    printf("%ld\n", count);
    if (count != NUM_THREADS) {
        pass1 = 0; 
    }
    mtx.unlock();
    return;
}

void thread_func_reusable(int var) {
    std::this_thread::sleep_for(std::chrono::microseconds(var)); //simulate work
    for (int i = 0; i < 3; i++) {
        mtx.lock();
        count++; 
        mtx.unlock();
        std::this_thread::sleep_for(std::chrono::microseconds(var));
        
        barrier.wait();
        
        mtx.lock();
        printf("%ld\n", count);
        if (count % NUM_THREADS) {
            pass2 = 0;
        }
        mtx.unlock();

        barrier.wait();
    }
    return; 
}

int main()
{
    std::cout << "Simple Barrier Test:" << std::endl;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(thread_func_throwaway, i);
    }
    for (auto &t : threads) {
        t.join();
    }
    threads.clear();

    std::cout << "Complex Barrier Test:" << std::endl;
    for (int i = 0; i < NUM_THREADS; i++) {
        threads.emplace_back(thread_func_reusable, i);
    }
    for (auto &t : threads) {
        t.join();
    }

    assert(pass1 == 1);
    assert(pass2 == 1);

    std::cout << "All barrier tests have passed" << std::endl; 
    return 0;
}