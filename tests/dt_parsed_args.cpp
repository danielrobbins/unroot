#include "actions/parsed_args.hpp"
#include "doctest.h"

using actions::ToBeParsedArgs;

TEST_CASE("action help is recognized before the command separator") {
    ToBeParsedArgs args;
    args.action_name = "enter";

    args.args = {"root", "--help", "--", "/bin/true"};
    CHECK(args.is_action_help());

    args.args = {"root", "-h", "--", "/bin/true"};
    CHECK(args.is_action_help());
}

TEST_CASE("command help arguments are opaque after the separator") {
    ToBeParsedArgs args;
    args.action_name = "enter";

    for (const auto& trailing :
         {std::vector<std::string>{"--", "/bin/true", "--help"},
          std::vector<std::string>{"root", "--", "/bin/true", "-h"},
          std::vector<std::string>{"root", "--", "/bin/true", "a", "-h", "b"},
          std::vector<std::string>{"root", "--", "/bin/true", "--", "--help"}}) {
        args.args = trailing;
        CHECK_FALSE(args.is_action_help());
    }
}

TEST_CASE("action help requires an action") {
    ToBeParsedArgs args;
    args.args = {"--help"};
    CHECK_FALSE(args.is_action_help());
}
