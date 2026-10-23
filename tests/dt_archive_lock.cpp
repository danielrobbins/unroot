#include "doctest.h"
#include "test_support.hpp"
#include "util/archive_lock.hpp"

#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using LockTree = test::TempDirectory;

} // namespace

TEST_CASE("archive lock elects one owner for a rootfs") {
    LockTree tree;
    auto owner = util::acquireArchiveLock(tree.root, false);
    REQUIRE(owner);
    CHECK(owner.matchesRoot());

    auto contender = util::acquireArchiveLock(tree.root, false);
    CHECK_FALSE(contender);
    CHECK(contender.busy);
    CHECK(contender.error.find("archive operation is already active") !=
          std::string::npos);
}

TEST_CASE("archive lock is reusable after release") {
    LockTree tree;
    {
        auto owner = util::acquireArchiveLock(tree.root, false);
        REQUIRE(owner);
    }

    auto next = util::acquireArchiveLock(tree.root, false);
    REQUIRE(next);
    CHECK_FALSE(next.busy);
}

TEST_CASE("archive lock is released when its process exits") {
    LockTree tree;
    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        auto lock = util::acquireArchiveLock(tree.root, false);
        _exit(lock ? 0 : 1);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    CHECK(util::acquireArchiveLock(tree.root, false));
}

TEST_CASE("archive lock detects root path replacement") {
    LockTree tree;
    const auto root = tree.root / "root";
    const auto moved = tree.root / "moved";
    std::filesystem::create_directory(root);
    auto lock = util::acquireArchiveLock(root, false);
    REQUIRE(lock);
    std::filesystem::rename(root, moved);
    std::filesystem::create_directory(root);

    CHECK_FALSE(lock.matchesRoot());
}

TEST_CASE("archive lock detects parent path replacement") {
    LockTree tree;
    const auto parent = tree.root / "parent";
    const auto root = parent / "root";
    const auto moved = tree.root / "moved";
    std::filesystem::create_directories(root);
    auto lock = util::acquireArchiveLock(root, false);
    REQUIRE(lock);
    std::filesystem::rename(parent, moved);
    std::filesystem::create_directories(root);

    CHECK_FALSE(lock.matchesRoot());
}

TEST_CASE("archive lock creates and pins a missing rootfs") {
    LockTree tree;
    const auto missing = tree.root / "root";
    auto owner = util::acquireArchiveLock(missing, true);

    REQUIRE(owner);
    CHECK(owner.created);
    CHECK(std::filesystem::is_directory(missing));
    CHECK(owner.matchesRoot());

    auto contender = util::acquireArchiveLock(missing, false);
    CHECK_FALSE(contender);
    CHECK(contender.busy);
    owner.preserveRoot();
}

TEST_CASE("archive lock reports an existing rootfs") {
    LockTree tree;
    auto owner = util::acquireArchiveLock(tree.root, true);

    REQUIRE(owner);
    CHECK_FALSE(owner.created);
    CHECK(owner.matchesRoot());
}

TEST_CASE("archive lock creates missing rootfs parents") {
    LockTree tree;
    const auto missing = tree.root / "one" / "two" / "root";
    auto owner = util::acquireArchiveLock(missing, true);

    REQUIRE(owner);
    CHECK(owner.created);
    CHECK(owner.matchesRoot());
    owner.preserveRoot();
}

TEST_CASE("archive lock removes an unused rootfs created for preflight") {
    LockTree tree;
    const auto missing = tree.root / "unused";
    {
        auto owner = util::acquireArchiveLock(missing, true);
        REQUIRE(owner);
        CHECK(owner.created);
    }

    CHECK_FALSE(std::filesystem::exists(missing));
}

TEST_CASE("archive lock cleanup preserves a replacement rootfs") {
    LockTree tree;
    const auto missing = tree.root / "root";
    const auto moved = tree.root / "moved";
    {
        auto owner = util::acquireArchiveLock(missing, true);
        REQUIRE(owner);
        std::filesystem::rename(missing, moved);
        std::filesystem::create_directory(missing);
    }

    CHECK(std::filesystem::is_directory(missing));
    CHECK(std::filesystem::is_directory(moved));
}
