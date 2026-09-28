// A shared library linked into shtest: constructors, TLS, C++ exceptions and
// an interposable function.
#include <stdexcept>
#include <string>
#include <vector>

int lib_ctor_ran;
thread_local int lib_tls = 17;
static std::vector<std::string> *log_entries;

__attribute__((constructor)) static void lib_init() { lib_ctor_ran = 1; log_entries = new std::vector<std::string>; }

extern "C" int lib_add(int a, int b) { return a + b; }
extern "C" int lib_weak_hook() { return 1; }                  /* shtest defines its own: interposition */
extern "C" int lib_call_hook() { return lib_weak_hook(); }
extern "C" int *lib_tls_addr() { return &lib_tls; }

void lib_log(const std::string &s) { log_entries->push_back(s); }
size_t lib_log_size() { return log_entries->size(); }
void lib_throw(const std::string &what) { throw std::runtime_error(what); }
