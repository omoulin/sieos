/*
 * cxxtest-basic (make cxx-test): C++ on SIEOS, built by clang with libc++,
 * libc++abi and libunwind on libposix: strings and containers, a static
 * object's constructor and destructor, virtual calls, exceptions thrown and
 * caught (through several frames, rethrown), RTTI, std::thread with mutex
 * and condition variable, atomics, std::chrono, std::function, iostreams,
 * std::filesystem, std::unique_ptr / shared_ptr, std::sort.
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

static int fails;
static void check(const char *what, bool ok)
{
    std::printf("cxxtest basic: %s: %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

static int built;
struct Global { Global() { built = 42; } ~Global() { std::printf("cxxtest basic: static destructor ran\n"); } } global;

struct Shape { virtual ~Shape() = default; virtual double area() const = 0; };
struct Square : Shape { double s; explicit Square(double x) : s(x) {} double area() const override { return s * s; } };
struct Circle : Shape { double r; explicit Circle(double x) : r(x) {} double area() const override { return 3.0 * r * r; } };

static int depth(int n) { if (!n) throw std::runtime_error("bottom"); return depth(n - 1) + 1; }

int main()
{
    check("a static object's constructor ran", built == 42);
    std::string s = "hello";
    s += ", world";
    std::vector<int> v{5, 3, 9, 1};
    std::sort(v.begin(), v.end());
    std::map<std::string, int> m{{"b", 2}, {"a", 1}};
    std::unordered_map<int, std::string> u{{1, "one"}};
    check("string, vector, sort, map, unordered_map",
          s == "hello, world" && v == std::vector<int>{1, 3, 5, 9} && m.begin()->first == "a" && u[1] == "one");
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Square>(2));
    shapes.push_back(std::make_unique<Circle>(1));
    double total = 0;
    for (auto &p : shapes) total += p->area();
    check("virtual calls, unique_ptr", total == 7.0);
    Shape &first = *shapes[0];
    check("RTTI", typeid(first) == typeid(Square) && dynamic_cast<Circle *>(shapes[1].get()) != nullptr &&
                  dynamic_cast<Circle *>(shapes[0].get()) == nullptr);
    std::string caught;
    try { depth(20); } catch (const std::exception &e) { caught = e.what(); }
    check("an exception through 20 frames", caught == "bottom");
    int rethrown = 0;
    try { try { throw 7; } catch (int) { throw; } } catch (int x) { rethrown = x; }
    check("rethrown", rethrown == 7);
    bool bad = false;
    try { (void)std::vector<int>().at(3); } catch (const std::out_of_range &) { bad = true; }
    check("the library's own exceptions", bad);
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<long> atom{0};
    long plain = 0;
    int ready = 0;
    std::vector<std::thread> ts;
    for (int i = 0; i < 16; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 10000; k++) { atom++; std::lock_guard<std::mutex> g(mu); plain++; }
            std::lock_guard<std::mutex> g(mu);
            ready++;
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return ready == 16; });
    }
    for (auto &t : ts) t.join();
    check("16 std::threads, mutex, condition_variable, atomic", atom == 160000 && plain == 160000);
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    check("chrono, sleep_for", ms >= 19 && ms < 500);
    std::function<int(int)> twice = [](int x) { return 2 * x; };
    auto shared = std::make_shared<int>(5);
    auto copy = shared;
    check("function, shared_ptr", twice(21) == 42 && shared.use_count() == 2);
    std::ostringstream os;
    os << "pi=" << 3.5 << " n=" << 12;
    check("ostringstream", os.str() == "pi=3.5 n=12");
    namespace fs = std::filesystem;
    fs::create_directories("/tmp/cxx/a/b");
    { std::ofstream f("/tmp/cxx/a/b/f.txt"); f << "data\n"; }
    std::ifstream in("/tmp/cxx/a/b/f.txt");
    std::string line;
    std::getline(in, line);
    int n = 0;
    for (auto &e : fs::recursive_directory_iterator("/tmp/cxx")) n += e.is_regular_file();
    check("filesystem, fstream", line == "data" && n == 1 && fs::file_size("/tmp/cxx/a/b/f.txt") == 5);
    fs::remove_all("/tmp/cxx");
    std::cout << "cxxtest basic: iostream works" << std::endl;
    std::printf("cxxtest basic: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
