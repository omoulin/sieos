/*
 * cxx_fs.cc
 *
 * Copyright (C) 2026 Olivier Moulin
 * Part of SIEOS, released under the GNU General Public License version 3
 * (GPL-3.0); see the LICENSE file.
 */
// std::filesystem on SIEOS (ext4 and tmpfs).
#include "check.h"
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;

int main()
{
	fs::path base = "/tmp/cxx_fs";
	fs::remove_all(base);
	CHECK(fs::create_directories(base / "a" / "b"));
	{ std::ofstream(base / "a" / "f.txt") << "12345"; }
	CHECK(fs::file_size(base / "a" / "f.txt") == 5);
	fs::create_symlink("a/f.txt", base / "link");
	CHECK(fs::is_symlink(fs::symlink_status(base / "link")) && fs::read_symlink(base / "link") == "a/f.txt");
	CHECK(fs::canonical(base / "link") == fs::path("/tmp/cxx_fs/a/f.txt"));
	int n = 0;
	for (auto &e : fs::recursive_directory_iterator(base)) { (void)e; n++; }
	CHECK(n == 4);
	fs::rename(base / "a" / "f.txt", base / "g.txt");
	CHECK(fs::exists(base / "g.txt") && !fs::exists(base / "a" / "f.txt"));
	CHECK(fs::current_path().is_absolute());
	auto sp = fs::space("/tmp");
	CHECK(sp.capacity > 0);
	CHECK(fs::remove_all(base) == 5);
	std::printf("cxx_fs: %s\n", t_fails ? "FAILED" : "ok");
	return DONE();
}
