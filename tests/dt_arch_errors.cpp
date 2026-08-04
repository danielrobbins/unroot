#include "doctest.h"
#include "arch.hpp"
#include <fstream>
#include <cstdio>
#include <string>
#include <filesystem>

TEST_CASE("readElfInfoAtPathEx non-existent path returns Io error") {
    auto r = readElfInfoAtPathEx("/no/such/definitely/not/present/____unroot_test____");
    CHECK(!r.ok());
    CHECK(r.error().code == util::LibErr::Io);
}

TEST_CASE("readElfInfoAtPathEx non-ELF regular file returns Invalid error") {
    std::string tmp = std::filesystem::temp_directory_path() / "unroot_nonelf_test.txt";
    {
        std::ofstream ofs(tmp);
        ofs << "hello"; // plain text, not ELF
    }
    auto r = readElfInfoAtPathEx(tmp);
    CHECK(!r.ok());
    CHECK(r.error().code == util::LibErr::Invalid);
    std::filesystem::remove(tmp);
}
