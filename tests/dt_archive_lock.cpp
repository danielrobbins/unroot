#include "doctest.h"
#include "util/archive_lock.hpp"

#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class LockTree {
public:
    LockTree() {
        char pattern[] = "/tmp/unroot-archive-lock-XXXXXX";
        char* created = ::mkdtemp(pattern);
        REQUIRE(created != nullptr);
        root = created;
    }

    ~LockTree() { std::filesystem::remove_all(root); }

    std::filesystem::path root;
};

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
    auto lock = util::acquireArchiveLock(tree.root, false);
    REQUIRE(lock);
    const auto moved = tree.root.string() + "-moved";
    std::filesystem::rename(tree.root, moved);
    std::filesystem::create_directory(tree.root);

    CHECK_FALSE(lock.matchesRoot());

    std::filesystem::remove_all(tree.root);
    std::filesystem::rename(moved, tree.root);
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
