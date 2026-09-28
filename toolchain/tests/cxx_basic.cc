// Containers, algorithms, strings, RTTI, virtual dispatch, templates.
#include "check.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <memory>

struct Base { virtual ~Base() = default; virtual int f() const { return 1; } };
struct Derived : Base { int f() const override { return 2; } };
template <typename T> T twice(T x) { return x + x; }

int main()
{
	std::vector<int> v(1000);
	std::iota(v.begin(), v.end(), 0);
	std::reverse(v.begin(), v.end());
	std::sort(v.begin(), v.end());
	CHECK(v.front() == 0 && v.back() == 999 && std::accumulate(v.begin(), v.end(), 0L) == 499500);
	std::map<std::string, int> m{{"b", 2}, {"a", 1}};
	CHECK(m.begin()->first == "a");
	std::unordered_map<int, std::string> u;
	for (int i = 0; i < 10000; i++) u[i] = std::to_string(i);
	CHECK(u.size() == 10000 && u[4242] == "4242");
	std::string s = "hello";
	s += ", world";
	CHECK(s.find("world") == 7 && s.substr(0, 5) == "hello");
	std::unique_ptr<Base> b = std::make_unique<Derived>();
	CHECK(b->f() == 2 && dynamic_cast<Derived *>(b.get()) && typeid(*b) == typeid(Derived));
	CHECK(twice(21) == 42 && twice(std::string("ab")) == "abab");
	std::cout << "cxx_basic: " << (t_fails ? "FAILED" : "ok") << std::endl;
	return DONE();
}
