/*
 * cxxtest-stress (make cxx-test): threads started and joined many times
 * over, with a mutex, a condition variable and atomics: what the browser's
 * thread pools do all day. 200 rounds of 16 threads.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

int main()
{
    long bad = 0;
    for (int round = 0; round < 200; round++) {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<long> atom{0};
        long plain = 0;
        int ready = 0;
        std::vector<std::thread> ts;
        for (int i = 0; i < 16; i++)
            ts.emplace_back([&] {
                for (int k = 0; k < 500; k++) { atom++; std::lock_guard<std::mutex> g(mu); plain++; }
                std::lock_guard<std::mutex> g(mu);
                ready++;
                cv.notify_one();
            });
        {
            std::unique_lock<std::mutex> l(mu);
            cv.wait(l, [&] { return ready == 16; });
        }
        for (auto &t : ts) t.join();
        bad += atom != 8000 || plain != 8000;
        if (round % 50 == 49) std::printf("cxxtest stress: %d rounds\n", round + 1);
    }
    std::printf("cxxtest stress: 200 rounds of 16 threads: %s\n", bad ? "FAIL" : "ok");
    std::printf("cxxtest stress: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
