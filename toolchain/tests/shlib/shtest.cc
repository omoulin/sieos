// A dynamically linked C++ program using libshtest.so and dlopen()ing plugin.so.
#include "../check.h"
#include <dlfcn.h>
#include <link.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <cstring>

extern int lib_ctor_ran;
extern "C" int lib_add(int, int);
extern "C" int lib_call_hook();
extern "C" int *lib_tls_addr();
void lib_log(const std::string &);
size_t lib_log_size();
void lib_throw(const std::string &);
extern "C" int lib_weak_hook() { return 2; }             /* interposes the library's */
int plugin_unload_count;

static int count_objects(struct dl_phdr_info *info, size_t, void *data)
{
	if (info->dlpi_name && std::strstr(info->dlpi_name, "libshtest.so"))
		++*(int *)data;
	return 0;
}

int main(int argc, char **argv)
{
	CHECK(lib_ctor_ran == 1);
	CHECK(lib_add(40, 2) == 42);
	CHECK(lib_call_hook() == 2);
	lib_log("a");
	lib_log("b");
	CHECK(lib_log_size() == 2);
	bool caught = false;
	try { lib_throw("across"); } catch (const std::runtime_error &e) { caught = std::string(e.what()) == "across"; }
	CHECK(caught);
	int main_tls = *lib_tls_addr(), thr_tls = 0;
	std::thread t([&] { *lib_tls_addr() = 99; thr_tls = *lib_tls_addr(); });
	t.join();
	CHECK(main_tls == 17 && thr_tls == 99 && *lib_tls_addr() == 17);
	int found = 0;
	dl_iterate_phdr(count_objects, &found);
	CHECK(found == 1);

	std::string dir = argv[0];
	dir = dir.substr(0, dir.rfind('/') + 1);
	void *h = dlopen((dir + "plugin.so").c_str(), RTLD_NOW);
	CHECK(h != nullptr);
	if (h) {
		auto tlsv = (int (*)())dlsym(h, "plugin_tls_value");
		auto name = (const char *(*)())dlsym(h, "plugin_get_name");
		auto thr = (void (*)())dlsym(h, "plugin_throw");
		CHECK(tlsv && name && thr);
		CHECK(tlsv() == 1234 && tlsv() == 1235);
		int other = 0;
		std::thread t2([&] { other = tlsv(); });
		t2.join();
		CHECK(other == 1234);
		CHECK(std::string(name()) == "plugin");
		bool pc = false;
		try { thr(); } catch (const std::logic_error &e) { pc = std::string(e.what()) == "from plugin"; }
		CHECK(pc);
		CHECK(dlsym(h, "no_such_symbol") == nullptr && dlerror() != nullptr);
		dlclose(h);
	}
	std::printf("shtest: %s\n", t_fails ? "FAILED" : "ok");
	return DONE();
}
