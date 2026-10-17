#include "doctest.h"

#include <filesystem>
#include <fstream>

#include "util/sha256.hpp"

TEST_CASE("sha256: hashes a pinned regular file") {
  const auto path = std::filesystem::temp_directory_path() /
                    "unroot-sha256-doctest";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "abc";
  }
  const auto result = util::sha256File(path);
  std::filesystem::remove(path);

  REQUIRE(result);
  CHECK(result.size == 3);
  CHECK(result.hex ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256: rejects a directory") {
  const auto result =
      util::sha256File(std::filesystem::temp_directory_path());
  CHECK_FALSE(result);
}
