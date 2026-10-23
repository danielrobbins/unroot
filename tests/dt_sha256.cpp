#include "doctest.h"
#include "test_support.hpp"

#include <filesystem>
#include <fstream>

#include "util/sha256.hpp"

TEST_CASE("sha256: hashes a pinned regular file") {
  test::TempDirectory fixture;
  const auto path = fixture.root / "sha256";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "abc";
  }
  const auto result = util::sha256File(path);

  REQUIRE(result);
  CHECK(result.size == 3);
  CHECK(result.hex ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256: rejects a directory") {
  test::TempDirectory fixture;
  const auto result = util::sha256File(fixture.root);
  CHECK_FALSE(result);
}
