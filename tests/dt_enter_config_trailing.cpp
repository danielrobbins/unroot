#include "doctest.h"
#include "actions/option_parser.hpp"
#include <vector>
#include <string>

using namespace actions;

// This test emulates the EnterConfig parser configuration relevant to trailing args
// without instantiating EnterConfig (avoids linking unrelated action/runtime symbols).

static OptionParser make_enter_like_parser(std::vector<std::string>& shell, std::string& root) {
    (void)shell; // shell not needed for parser construction in this emulation
    OptionParser p;
    p.allow_trailing_args();
    p.add_positional_meta("ROOT", "Root fs", [&](const std::string& v){ root = v; });
    return p;
}

TEST_CASE("Enter-like parser: collects trailing command after --") {
    std::vector<std::string> shell; std::string root;
    auto p = make_enter_like_parser(shell, root);
    auto r = p.parse({"/fake/root", "--", "/bin/echo", "hello", "world"});
    for (auto& s : r.trailing_args) shell.push_back(s);
    REQUIRE(shell.size() == 3);
    CHECK(shell[0] == "/bin/echo");
    CHECK(shell[1] == "hello");
    CHECK(shell[2] == "world");
    CHECK(root == "/fake/root");
}

TEST_CASE("Enter-like parser: implicit trailing without -- is collected") {
    std::vector<std::string> shell; std::string root;
    auto p = make_enter_like_parser(shell, root);
    auto r = p.parse({"/fake/root", "/bin/echo"});
    CHECK(r.positionals.size() == 1);
    CHECK(r.trailing_args.size() == 1);
    CHECK(r.trailing_args[0] == "/bin/echo");
}
