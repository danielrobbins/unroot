#include "doctest.h"
#include "actions/enter_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"

using namespace actions;

TEST_CASE("enter consumes a rootfs ownership mode from metadata") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    CHECK(config.root == "/tmp/rootfs");
    CHECK_FALSE(config.singleId);
    CHECK_FALSE(config.native);
}

TEST_CASE("enter accepts explicit single-ID ownership") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--single", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    CHECK(config.singleId);
    CHECK_FALSE(config.native);
}

TEST_CASE("enter rejects contradictory ownership options") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--single", "--native", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    REQUIRE_THROWS_AS(config.validate(), AppException);
}

TEST_CASE("enter accepts explicit native ownership") {
    ToBeParsedArgs args;
    args.action_name = "enter";
    args.args = {"/tmp/rootfs", "--native", "--", "/bin/true"};
    EnterConfig config;
    config.parse(args);

    CHECK(config.native);
}

TEST_CASE("enter does not expose retired rootless or ID-map options") {
    for (const auto& option : {"--rootless", "--idmap=user"}) {
        ToBeParsedArgs args;
        args.action_name = "enter";
        args.args = {"/tmp/rootfs", option, "--", "/bin/true"};
        EnterConfig config;
        REQUIRE_THROWS_AS(config.parse(args), std::invalid_argument);
    }
}
