/*
 * cxx_except.cc
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
// Exceptions: unwinding through frames, rethrow, exception_ptr, bad_alloc, nested, destructors run.
#include "check.h"
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

static int dtors;
struct Guard { ~Guard() { dtors++; } };
[[gnu::noinline]] static void deep(int n) { Guard g; if (!n) throw std::runtime_error("bottom"); deep(n - 1); }
struct Thrower { Thrower() { throw 7; } };

int main()
{
	try { deep(50); CHECK(false); } catch (const std::exception &e) { CHECK(std::string(e.what()) == "bottom"); }
	CHECK(dtors == 51);
	std::exception_ptr p;
	try { throw std::logic_error("saved"); } catch (...) { p = std::current_exception(); }
	try { std::rethrow_exception(p); } catch (const std::logic_error &e) { CHECK(std::string(e.what()) == "saved"); }
	try { try { throw 1; } catch (int) { throw; } } catch (int x) { CHECK(x == 1); }
	try { Thrower t; (void)t; } catch (int x) { CHECK(x == 7); }
	bool bad = false;
	try { void *volatile huge = ::operator new(size_t(1) << 46); ::operator delete(huge); } catch (const std::bad_alloc &) { bad = true; }
	CHECK(bad);
	try {
		try { throw std::out_of_range("inner"); }
		catch (...) { std::throw_with_nested(std::runtime_error("outer")); }
	} catch (const std::runtime_error &e) {
		try { std::rethrow_if_nested(e); } catch (const std::out_of_range &i) { CHECK(std::string(i.what()) == "inner"); }
	}
	std::printf("cxx_except: %s\n", t_fails ? "FAILED" : "ok");
	return DONE();
}
