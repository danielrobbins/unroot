#include "doctest.h"
#include "test_support.hpp"

#include <fstream>

TEST_CASE("temporary test directories are distinct and inside the build tree") {
  test::TempDirectory first;
  test::TempDirectory second;
  CHECK(first.root != second.root);
  CHECK(first.root.parent_path() ==
        std::filesystem::absolute(UNROOT_TEST_BUILD_DIR));
  CHECK(std::filesystem::is_directory(second.root));
}

TEST_CASE("temporary test directories clean up after a failed assertion") {
  std::filesystem::path root;
  bool caught = false;
  try {
    test::TempDirectory tree;
    root = tree.root;
    std::ofstream(root / "leftover") << "temporary";
    REQUIRE(false);
  } catch (const std::exception&) {
    caught = true;
  }
  REQUIRE(caught);
  REQUIRE(!root.empty());
  CHECK_FALSE(std::filesystem::exists(root));
}

TEST_CASE("temporary test directories clean up without following symlinks") {
  test::TempDirectory outside;
  std::ofstream(outside.root / "keep") << "keep";
  std::filesystem::path removed;
  {
    test::TempDirectory inside;
    removed = inside.root;
    std::filesystem::create_directory_symlink(outside.root, inside.root / "link");
  }
  CHECK_FALSE(std::filesystem::exists(removed));
  CHECK(std::filesystem::exists(outside.root / "keep"));
}

TEST_CASE("environment fixtures restore unset empty and populated variables") {
  for (const char* original : {static_cast<const char*>(nullptr), "", "original"}) {
    test::ScopedEnvironment baseline("UNROOT_TEST_ENV", original);
    try {
      test::ScopedEnvironment changed("UNROOT_TEST_ENV", "temporary");
      REQUIRE(false);
    } catch (const std::exception&) {
    }
    const char* restored = ::getenv("UNROOT_TEST_ENV");
    if (original) {
      REQUIRE(restored != nullptr);
      CHECK(std::string(restored) == original);
    } else {
      CHECK(restored == nullptr);
    }
  }
}
