#include "doctest.h"
#include "util/subid.hpp"
#include "util/subid_backend.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace util;

namespace {

class TempTree {
public:
    TempTree() {
        char pattern[] = "/tmp/unroot-subid-XXXXXX";
        char* created = ::mkdtemp(pattern);
        REQUIRE(created != nullptr);
        root = created;
    }

    ~TempTree() { std::filesystem::remove_all(root); }

    std::filesystem::path write(const std::string& name,
                                const std::string& content,
                                bool executable = false) const {
        auto path = root / name;
        std::ofstream(path) << content;
        if (executable) REQUIRE(::chmod(path.c_str(), 0700) == 0);
        return path;
    }

    std::filesystem::path root;
};

} // namespace

TEST_CASE("file subid backend selects matching username ranges") {
    TempTree tree;
    auto uids = tree.write("subuid", "other:10:20\nalice:1000:64\n");
    auto gids = tree.write("subgid", "alice:2000:64\n");

    auto result = resolveFileSubIds("alice", 1001, 16, uids, gids);

    REQUIRE(result);
    CHECK(result.allocation.uidStart == 1000);
    CHECK(result.allocation.gidStart == 2000);
    CHECK(result.allocation.count == 16);
    CHECK(result.allocation.source == "files");
}

TEST_CASE("file subid backend accepts numeric owner records") {
    TempTree tree;
    auto uids = tree.write("subuid", "1001:3000:32\n");
    auto gids = tree.write("subgid", "1001:4000:32\n");

    auto result = resolveFileSubIds("alice", 1001, 32, uids, gids);

    REQUIRE(result);
    CHECK(result.allocation.uidStart == 3000);
    CHECK(result.allocation.gidStart == 4000);
}

TEST_CASE("file subid backend skips malformed insufficient and overflowing ranges") {
    TempTree tree;
    auto uids = tree.write(
        "subuid",
        "alice:bad:32\nalice:100:15\nalice:4294967290:16\nalice:5000:16\n");
    auto gids = tree.write(
        "subgid",
        "alice:200:0\nalice:6000:16:extra\nalice:7000:16\n");

    auto result = resolveFileSubIds("alice", 1001, 16, uids, gids);

    REQUIRE(result);
    CHECK(result.allocation.uidStart == 5000);
    CHECK(result.allocation.gidStart == 7000);
}

TEST_CASE("file subid backend reports missing and undersized allocations") {
    TempTree tree;
    auto empty = tree.write("empty", "other:1000:64\n");
    auto shortRange = tree.write("short", "alice:2000:4\n");

    auto missing = resolveFileSubIds("alice", 1001, 8, empty, empty);
    CHECK_FALSE(missing);
    CHECK(missing.error.find("no subordinate UID ranges") != std::string::npos);

    auto undersized = resolveFileSubIds("alice", 1001, 8, shortRange,
                                        shortRange);
    CHECK_FALSE(undersized);
    CHECK(undersized.error.find("no subordinate UID range can map 8 IDs") !=
          std::string::npos);
}

TEST_CASE("file subid backend validates an exact recorded allocation") {
    TempTree tree;
    auto uids = tree.write("subuid", "alice:1000:64\n");
    auto gids = tree.write("subgid", "alice:2000:64\n");

    auto valid = validateFileSubIds(
        "alice", 1001, {1016, 2032, 16, {}}, uids, gids);
    REQUIRE(valid);
    CHECK(valid.allocation.uidStart == 1016);
    CHECK(valid.allocation.gidStart == 2032);

    auto staleUid = validateFileSubIds(
        "alice", 1001, {1060, 2032, 16, {}}, uids, gids);
    CHECK_FALSE(staleUid);
    CHECK(staleUid.error.find("UID range is no longer assigned") !=
          std::string::npos);

    auto staleGid = validateFileSubIds(
        "alice", 1001, {1016, 2060, 16, {}}, uids, gids);
    CHECK_FALSE(staleGid);
    CHECK(staleGid.error.find("GID range is no longer assigned") !=
          std::string::npos);
}

TEST_CASE("subid allocation client accepts one exact protocol record") {
    TempTree tree;
    auto helper = tree.write(
        "unroot-util",
        "#!/bin/sh\nprintf '%s\\n' 'unroot-idmap-v1 1000 2000 16 files'\n",
        true);

    auto result = querySubIdAllocation(16, helper);

    REQUIRE(result);
    CHECK(result.allocation.uidStart == 1000);
    CHECK(result.allocation.gidStart == 2000);
    CHECK(result.allocation.count == 16);
    CHECK(result.allocation.source == "files");
}

TEST_CASE("subid allocation client validates the recorded protocol values") {
    TempTree tree;
    auto helper = tree.write(
        "unroot-util",
        "#!/bin/sh\nprintf '%s\\n' 'unroot-idmap-v1 1000 2000 16 files'\n",
        true);

    auto result = validateSubIdAllocation({1000, 2000, 16, {}}, helper);
    REQUIRE(result);
    CHECK(result.allocation.source == "files");

    CHECK_FALSE(validateSubIdAllocation({1001, 2000, 16, {}}, helper));
    CHECK_FALSE(validateSubIdAllocation({1000, 2001, 16, {}}, helper));
    CHECK_FALSE(validateSubIdAllocation({1000, 2000, 15, {}}, helper));
    CHECK_FALSE(validateSubIdAllocation({UINT_MAX - 1, 2000, 16, {}}, helper));
}

TEST_CASE("subid allocation client works with closed output descriptors") {
    TempTree tree;
    auto helper = tree.write(
        "unroot-util",
        "#!/bin/sh\necho 'unroot-idmap-v1 1000 2000 16 files'\n", true);

    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        ::close(STDOUT_FILENO);
        ::close(STDERR_FILENO);
        _exit(querySubIdAllocation(16, helper) ? 0 : 1);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("subid allocation client rejects malformed or mismatched responses") {
    TempTree tree;
    auto malformed = tree.write(
        "malformed", "#!/bin/sh\necho 'unroot-idmap-v0 1000 2000 16 files'\n",
        true);
    auto trailing = tree.write(
        "trailing",
        "#!/bin/sh\necho 'unroot-idmap-v1 1000 2000 16 files extra'\n",
        true);
    auto wrongCount = tree.write(
        "count", "#!/bin/sh\necho 'unroot-idmap-v1 1000 2000 15 files'\n",
        true);

    CHECK_FALSE(querySubIdAllocation(16, malformed));
    CHECK_FALSE(querySubIdAllocation(16, trailing));
    CHECK_FALSE(querySubIdAllocation(16, wrongCount));
}

TEST_CASE("subid allocation client propagates bounded helper failures") {
    TempTree tree;
    auto failing = tree.write(
        "failing", "#!/bin/sh\necho 'error: provider unavailable' >&2\nexit 1\n",
        true);
    auto oversized = tree.write(
        "oversized", "#!/bin/sh\nprintf '%s' '" + std::string(5000, 'x') +
                         "'\n",
        true);

    auto failure = querySubIdAllocation(16, failing);
    CHECK_FALSE(failure);
    CHECK(failure.error == "error: provider unavailable");

    auto excess = querySubIdAllocation(16, oversized);
    CHECK_FALSE(excess);
    CHECK(excess.error == "error: oversized response from unroot-util");
}

TEST_CASE("subid allocation client validates count and helper availability") {
    TempTree tree;
    auto missing = tree.root / "missing";

    CHECK_FALSE(querySubIdAllocation(0, missing));
    CHECK_FALSE(querySubIdAllocation(65536, missing));
    auto result = querySubIdAllocation(16, missing);
    CHECK_FALSE(result);
    CHECK(result.error.find("requires unroot-util installed next to unroot") !=
          std::string::npos);
}
