#include "doctest.h"
#include "util/path.hpp"
#include <filesystem>
#include <fstream>
#include <string>

using namespace util;

static void make_exec(const std::filesystem::path& p) {
    std::ofstream(p.string()).put('\n');
    ::chmod(p.c_str(), 0755);
}

TEST_CASE("findOnPath basic lookup skips relative and finds first executable") {
    // create a temp dir structure
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "unroot_ut_path1";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / "bin1");
    std::filesystem::create_directories(tmp / "bin2");
    make_exec(tmp/"bin2"/"tool");
    // bin1 intentionally missing tool, bin2 has it
    std::string newPath = (tmp/"bin1").string() + ":" + (tmp/"bin2").string();
    setenv("PATH", newPath.c_str(), 1);
    std::string found = findOnPath("tool");
    REQUIRE(!found.empty());
    REQUIRE(found.find("bin2/tool") != std::string::npos);
}

TEST_CASE("findOnPath returns empty when not found") {
    std::filesystem::path tmp = std::filesystem::temp_directory_path() / "unroot_ut_path2";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp/"binA");
    std::string newPath = (tmp/"binA").string();
    setenv("PATH", newPath.c_str(), 1);
    REQUIRE(findOnPath("idontexist").empty());
}

TEST_CASE("resolveExecInRoot relative path success and permission checks") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "unroot_ut_root1";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "sub" / "dir");
    // create executable relative to cwdInRoot
    fs::path exe = tmp/"sub"/"dir"/"prog";
    make_exec(exe);
    std::string err;
    // For a bare name (no slash) pathEnv must be provided; include '.' so cwdInRoot is considered.
    std::string resolved = resolveExecInRoot(tmp.string(), "/sub/dir", "prog", ".", &err);
    REQUIRE(err.empty());
    REQUIRE(resolved == "/sub/dir/prog");
}

TEST_CASE("resolveExecInRoot relative path missing executable") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "unroot_ut_root2";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "sub" / "dir");
    std::string err;
    std::string resolved = resolveExecInRoot(tmp.string(), "/sub/dir", "prog", ".", &err);
    REQUIRE(resolved.empty());
    REQUIRE(err.find("not found") != std::string::npos);
}

TEST_CASE("resolveExecInRoot PATH search with dot and absolute entries") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "unroot_ut_root3";
    fs::remove_all(tmp);
    fs::create_directories(tmp/"a");
    fs::create_directories(tmp/"b");
    make_exec(tmp/"b"/"tool");
    std::string err;
    std::string pathEnv = std::string(".:/b:/c"); // . should map to cwdInRoot, /b is absolute inside root
    std::string resolved = resolveExecInRoot(tmp.string(), "/", "tool", pathEnv, &err);
    REQUIRE(err.empty());
    REQUIRE(resolved == "/b/tool");
}

TEST_CASE("resolveExecInRoot PATH search fails") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "unroot_ut_root4";
    fs::remove_all(tmp);
    std::string err;
    std::string pathEnv = ":."; // empty and dot only, but no cwdInRoot entries
    std::string resolved = resolveExecInRoot(tmp.string(), "", "tool", pathEnv, &err);
    REQUIRE(resolved.empty());
    REQUIRE(err.find("not found") != std::string::npos);
}
