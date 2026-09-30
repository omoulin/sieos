/*
 * cxx_thread.cc
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
// std::thread, mutex, condition_variable, future/async, atomics, thread_local, jthread.
#include "check.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>
#include <stdexcept>

static std::atomic<int> tl_dtors{0};
struct TL { int v = 0; ~TL() { tl_dtors++; } };
thread_local TL tl;

int main()
{
	std::mutex m;
	long counter = 0;
	std::vector<std::thread> ts;
	for (int i = 0; i < 8; i++)
		ts.emplace_back([&] { for (int k = 0; k < 10000; k++) { std::lock_guard<std::mutex> g(m); counter++; } tl.v = 1; });
	for (auto &t : ts) t.join();
	CHECK(counter == 80000);
	CHECK(tl_dtors == 8);
	std::condition_variable cv;
	bool ready = false;
	std::thread w([&] { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return ready; }); });
	{ std::lock_guard<std::mutex> g(m); ready = true; }
	cv.notify_one();
	w.join();
	auto f = std::async(std::launch::async, [] { return 6 * 7; });
	CHECK(f.get() == 42);
	auto fe = std::async(std::launch::async, [] () -> int { throw std::runtime_error("in thread"); });
	bool caught = false;
	try { fe.get(); } catch (const std::runtime_error &) { caught = true; }
	CHECK(caught);
	std::atomic<long> a{0};
	std::thread t1([&] { for (int i = 0; i < 100000; i++) a.fetch_add(1); });
	std::thread t2([&] { for (int i = 0; i < 100000; i++) a.fetch_add(1); });
	t1.join(); t2.join();
	CHECK(a == 200000);
	std::atomic<bool> stopped{false};
	{
		std::jthread j([&](std::stop_token st) { while (!st.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(5)); stopped = true; });
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	CHECK(stopped);
	std::timed_mutex tm;
	tm.lock();
	auto t0 = std::chrono::steady_clock::now();
	std::thread tt([&] { CHECK(!tm.try_lock_for(std::chrono::milliseconds(50))); });
	tt.join();
	CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(40));
	tm.unlock();
	std::printf("cxx_thread: %s (hardware_concurrency %u)\n", t_fails ? "FAILED" : "ok", std::thread::hardware_concurrency());
	return DONE();
}
