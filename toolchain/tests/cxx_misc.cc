// regex, format, chrono, random, optional/variant/any, function, smart pointers, static objects.
#include "check.h"
#include <any>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <variant>
#include <cstdlib>

static int order;
struct Static { int n; Static() : n(++order) {} ~Static() { if (n != 1) std::abort(); } };
static Static first;

int main()
{
	std::regex re(R"((\w+)@(\w+)\.com)");
	std::smatch m;
	std::string s = "mail: bob@example.com!";
	CHECK(std::regex_search(s, m, re) && m[1] == "bob" && m[2] == "example");
	CHECK(std::format("{:>6}|{:#x}|{:.2f}", "ab", 255, 2.5) == "    ab|0xff|2.50");
	auto t0 = std::chrono::steady_clock::now();
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	auto dt = std::chrono::steady_clock::now() - t0;
	CHECK(dt >= std::chrono::milliseconds(25) && dt < std::chrono::seconds(2));
	auto now = std::chrono::system_clock::now();
	CHECK(std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() > 1600000000);
	std::mt19937 gen(42);
	std::uniform_int_distribution<int> dist(1, 6);
	int sum = 0;
	for (int i = 0; i < 1000; i++) sum += dist(gen);
	CHECK(sum > 3000 && sum < 4000);
	std::random_device rd;
	(void)rd();
	std::optional<int> o = 5;
	std::variant<int, std::string> v = std::string("x");
	std::any a = 3.5;
	CHECK(*o == 5 && std::get<std::string>(v) == "x" && std::any_cast<double>(a) == 3.5);
	std::function<int(int)> f = [k = 10](int x) { return x + k; };
	CHECK(f(5) == 15);
	auto sp = std::make_shared<int>(9);
	std::weak_ptr<int> wp = sp;
	CHECK(wp.lock() && *wp.lock() == 9);
	sp.reset();
	CHECK(wp.expired());
	CHECK(first.n == 1);
	std::printf("cxx_misc: %s\n", t_fails ? "FAILED" : "ok");
	return DONE();
}
