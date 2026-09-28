// iostreams: files, string streams, formatting, getline.
#include "check.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdio>

int main()
{
	const char *path = "/tmp/cxx_io.txt";
	{
		std::ofstream o(path);
		CHECK(o.good());
		o << "line one\n" << 42 << ' ' << std::fixed << std::setprecision(3) << 3.14159 << "\n";
	}
	std::ifstream i(path);
	std::string l1;
	int n = 0;
	double d = 0;
	std::getline(i, l1);
	i >> n >> d;
	CHECK(l1 == "line one" && n == 42 && d > 3.141 && d < 3.143);
	std::ostringstream os;
	os << std::hex << std::showbase << 255 << ' ' << std::setw(6) << std::setfill('*') << std::dec << 7;
	CHECK(os.str() == "0xff *****7");
	std::istringstream is("10 20 30");
	int a, b, c;
	is >> a >> b >> c;
	CHECK(a + b + c == 60);
	std::remove(path);
	std::cout << "cxx_io: " << (t_fails ? "FAILED" : "ok") << std::endl;
	return DONE();
}
