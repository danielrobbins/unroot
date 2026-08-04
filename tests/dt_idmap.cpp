#include "doctest.h"
#include "util/idmap.hpp"

#include <climits>

using namespace util;

TEST_CASE("single ID map contains one exact root extent") {
    auto plan = makeSingleIdMap(1000, 2000);

    CHECK(validateIdMapPlan(plan).empty());
    REQUIRE(plan.uids.size() == 1);
    CHECK(plan.uids[0].inside == 0);
    CHECK(plan.uids[0].outside == 1000);
    CHECK(plan.uids[0].count == 1);
    CHECK(subordinateIdCount(plan) == 0);
}

TEST_CASE("rich ID map contains root and contiguous subordinate extents") {
    auto plan = makeRichIdMap(1000, 2000, 100000, 200000, 65535, "files");

    CHECK(validateIdMapPlan(plan).empty());
    REQUIRE(plan.uids.size() == 2);
    CHECK(plan.uids[1].inside == 1);
    CHECK(plan.uids[1].outside == 100000);
    CHECK(plan.gids[1].outside == 200000);
    CHECK(subordinateIdCount(plan) == 65535);
}

TEST_CASE("native ownership contains no kernel ID-map extents") {
    auto plan = makeNativeIdMap();

    CHECK(validateIdMapPlan(plan).empty());
    CHECK(plan.mode == IdMapMode::Native);
    CHECK(plan.uids.empty());
    CHECK(plan.gids.empty());
    CHECK(subordinateIdCount(plan) == 0);

    plan.uids.push_back({0, 0, 1});
    CHECK_FALSE(validateIdMapPlan(plan).empty());
}

TEST_CASE("ID map rejects fragmented and inconsistent subordinate layouts") {
    auto fragmented = makeRichIdMap(1000, 1000, 100000, 100000, 16, "files");
    fragmented.uids.push_back({17, 200000, 16});
    CHECK_FALSE(validateIdMapPlan(fragmented).empty());

    auto mismatched = makeRichIdMap(1000, 1000, 100000, 100000, 16, "files");
    mismatched.gids[1].count = 15;
    CHECK_FALSE(validateIdMapPlan(mismatched).empty());

    auto gap = makeRichIdMap(1000, 1000, 100000, 100000, 16, "files");
    gap.uids[1].inside = 2;
    CHECK_FALSE(validateIdMapPlan(gap).empty());

    auto overlap = makeRichIdMap(1000, 1000, 1000, 1000, 16, "files");
    CHECK_FALSE(validateIdMapPlan(overlap).empty());
}

TEST_CASE("ID map rejects overflowing extents") {
    auto plan = makeRichIdMap(1000, 1000, UINT_MAX, 100000, 2, "files");
    CHECK_FALSE(validateIdMapPlan(plan).empty());
}
