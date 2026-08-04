#include "doctest.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "util/rootfs.hpp"

namespace fs = std::filesystem;

namespace {

struct TemporaryTree {
  TemporaryTree() {
    char pattern[] = "/tmp/unroot-rootfs-test-XXXXXX";
    char* created = ::mkdtemp(pattern);
    REQUIRE(created != nullptr);
    base = created;
    root = base / "root";
    outside = base / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
  }

  ~TemporaryTree() { fs::remove_all(base); }

  fs::path base;
  fs::path root;
  fs::path outside;
};

}  // namespace

TEST_CASE("Rootfs creates paths beneath its pinned root") {
  TemporaryTree tree;
  util::Rootfs root(tree.root.string());

  CHECK(root);
  CHECK(root.directory("/.unroot/bin", true));
  CHECK(fs::is_directory(tree.root / ".unroot" / "bin"));
}

TEST_CASE("Rootfs rejects a symlinked parent") {
  TemporaryTree tree;
  fs::create_directory_symlink(tree.outside, tree.root / ".unroot");
  util::Rootfs root(tree.root.string());

  CHECK_FALSE(root.directory("/.unroot/bin", true));
  CHECK_FALSE(fs::exists(tree.outside / "bin"));
  CHECK_FALSE(root.writeTextAtomic("/.unroot/meta.json", "unsafe"));
  CHECK_FALSE(fs::exists(tree.outside / "meta.json"));
}

TEST_CASE("Rootfs atomic writes replace rather than follow final symlinks") {
  TemporaryTree tree;
  fs::path sentinel = tree.outside / "sentinel";
  std::ofstream(sentinel) << "unchanged";
  fs::create_directories(tree.root / ".unroot");
  fs::create_symlink(sentinel, tree.root / ".unroot" / "meta.json");
  util::Rootfs root(tree.root.string());

  CHECK(root.writeTextAtomic("/.unroot/meta.json", "metadata"));
  CHECK(fs::is_regular_file(tree.root / ".unroot" / "meta.json"));
  std::ifstream input(sentinel);
  std::string value;
  input >> value;
  CHECK(value == "unchanged");
}

TEST_CASE("Rootfs mount targets reject final symlinks") {
  TemporaryTree tree;
  fs::path sentinel = tree.outside / "sentinel";
  std::ofstream(sentinel) << "unchanged";
  fs::create_directories(tree.root / "tmp");
  fs::create_symlink(sentinel, tree.root / "tmp" / "mapped");
  util::Rootfs root(tree.root.string());

  CHECK_FALSE(root.mountTarget("/tmp/mapped", false));
}

TEST_CASE("Rootfs atomic copies replace rather than modify hard links") {
  TemporaryTree tree;
  fs::path source = tree.base / "source";
  fs::path sentinel = tree.outside / "sentinel";
  std::ofstream(source) << "replacement";
  std::ofstream(sentinel) << "unchanged";
  fs::create_directories(tree.root / ".unroot" / "bin");
  fs::create_hard_link(sentinel, tree.root / ".unroot" / "bin" / "wrapper");
  util::Rootfs root(tree.root.string());

  CHECK(root.copyHostFileAtomic(source.string(), "/.unroot/bin/wrapper"));

  std::string sentinelValue;
  std::ifstream(sentinel) >> sentinelValue;
  CHECK(sentinelValue == "unchanged");
  std::string wrapperValue;
  std::ifstream(tree.root / ".unroot" / "bin" / "wrapper") >> wrapperValue;
  CHECK(wrapperValue == "replacement");
}

TEST_CASE("Rootfs resolves absolute symlinks inside the pinned root") {
  TemporaryTree tree;
  fs::create_directories(tree.root / "bin");
  std::ofstream(tree.root / "inside") << "inside";
  std::ofstream(tree.base / "inside") << "outside";
  fs::create_symlink("/inside", tree.root / "bin" / "tool");
  util::Rootfs root(tree.root.string());

  UniqueFd file = root.resolvedFile("/bin/tool");
  REQUIRE(file);
  char content[7]{};
  REQUIRE(::read(file.get(), content, 6) == 6);
  CHECK(std::string(content) == "inside");
}

TEST_CASE("Rootfs clamps parent traversal from symlink targets") {
  TemporaryTree tree;
  fs::create_directories(tree.root / "bin");
  std::ofstream(tree.base / "outside-secret") << "outside";
  fs::create_symlink("../../outside-secret", tree.root / "bin" / "tool");
  util::Rootfs root(tree.root.string());

  CHECK_FALSE(root.resolvedFile("/bin/tool"));
}

TEST_CASE("Rootfs detects symlink loops") {
  TemporaryTree tree;
  fs::create_symlink("/second", tree.root / "first");
  fs::create_symlink("/first", tree.root / "second");
  util::Rootfs root(tree.root.string());

  CHECK_FALSE(root.resolvedFile("/first"));
  CHECK(errno == ELOOP);
}
