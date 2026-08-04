// doctest version of compat blacklist tests (ported from Catch2)
#include "../third_party/doctest/doctest.h"
#include "compat_blacklist.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace compat;

TEST_CASE("compat: parseQemuVersionLine extracts version components") {
  auto v = parseQemuVersionLine("qemu-aarch64 version 7.1.0");
  REQUIRE(v.size() == 3);
  CHECK(v[0] == 7);
  CHECK(v[1] == 1);
  CHECK(v[2] == 0);
  auto v2 = parseQemuVersionLine("QEMU emulator version 8.0");
  REQUIRE(v2.size() == 2);
  CHECK(v2[0] == 8);
  CHECK(v2[1] == 0);
  auto v3 = parseQemuVersionLine("no digits here");
  CHECK(v3.empty());
}

TEST_CASE("compat: emulator probing does not invoke a shell") {
  auto directory = std::filesystem::temp_directory_path() /
                   ("unroot-compat-" + std::to_string(::getpid()));
  std::filesystem::create_directory(directory);
  auto executable = directory / "qemu;echo-shell";
  {
    std::ofstream script(executable);
    script << "#!/bin/sh\nprintf 'qemu-test version 9.8.7\\nignored\\n' >&2\n";
  }
  REQUIRE(::chmod(executable.c_str(), 0700) == 0);

  CHECK(probeVersionLine(executable.string()) == "qemu-test version 9.8.7");
  std::filesystem::remove_all(directory);
}

TEST_CASE("compat: versionPrefixMatches logic") {
  std::vector<int> actual{7,1,0};
  CHECK(versionPrefixMatches({}, actual));
  CHECK(versionPrefixMatches({7}, actual));
  CHECK(versionPrefixMatches({7,1}, actual));
  CHECK(versionPrefixMatches({7,1,0}, actual));
  CHECK_FALSE(versionPrefixMatches({7,2}, actual));
  CHECK_FALSE(versionPrefixMatches({7,1,1}, actual));
  CHECK_FALSE(versionPrefixMatches({7,1,0,1}, actual));
}

TEST_CASE("compat: evaluateSynthetic WSL2 warning and exact blacklist") {
  auto warnOnly = evaluateSynthetic("6.6.87.2-microsoft-standard-WSL2", {7,1,1});
  CHECK_FALSE(warnOnly.blacklisted);
  CHECK_FALSE(warnOnly.warnings.empty());
  auto bl = evaluateSynthetic("6.6.87.2-microsoft-standard-WSL2", {7,1,0});
  CHECK(bl.blacklisted);
  CHECK_FALSE(bl.blacklistReason.empty());
}
