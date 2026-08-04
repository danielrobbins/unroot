#include "doctest.h"
#include "actions/option_parser.hpp"
#include <vector>
#include <string>

using namespace actions;

TEST_CASE("OptionParser: trailing args forbidden by default") {
    OptionParser p; std::string first; p.add_positional_meta("A","first",[&](const std::string& v){ first=v; });
    // Extra positional without opting in should error (reported as unexpected positional)
    REQUIRE_THROWS_AS(p.parse({"one","two"}), std::invalid_argument);
}

TEST_CASE("OptionParser: allow trailing implicit extras without --") {
    OptionParser p; p.allow_trailing_args(); std::string a; p.add_positional_meta("A","a",[&](const std::string& v){ a=v; });
    auto r = p.parse({"one","two","three"});
    CHECK(r.positionals.size()==1);
    CHECK(r.positionals[0]=="one");
    CHECK(r.trailing_args.size()==2);
    CHECK(r.trailing_args[0]=="two");
    CHECK(r.trailing_args[1]=="three");
}

TEST_CASE("OptionParser: explicit -- requires at least one trailing arg") {
    OptionParser p; p.allow_trailing_args(); std::string a; p.add_positional_meta("A","a",[&](const std::string& v){ a=v; });
    REQUIRE_THROWS_AS(p.parse({"one","--"}), std::invalid_argument);
}

TEST_CASE("OptionParser: explicit -- collects trailing args") {
    OptionParser p; p.allow_trailing_args(); std::string a; p.add_positional_meta("A","a",[&](const std::string& v){ a=v; });
    auto r = p.parse({"one","--","two","three"});
    CHECK(r.positionals.size()==1);
    CHECK(r.trailing_args.size()==2);
    CHECK(r.trailing_args[0]=="two");
    CHECK(r.trailing_args[1]=="three");
}

TEST_CASE("OptionParser: explicit -- permits an omitted optional positional") {
    OptionParser p; p.allow_trailing_args(); std::string a;
    p.add_positional_meta("A", "a", [&](const std::string& v){ a=v; }, false);
    auto r = p.parse({"--", "command"});
    CHECK(r.positionals.empty());
    REQUIRE(r.trailing_args.size() == 1);
    CHECK(r.trailing_args[0] == "command");
    CHECK(a.empty());
}

TEST_CASE("OptionParser: explicit -- rejects extras beyond declared positionals") {
    OptionParser p; p.allow_trailing_args(); std::string a;
    p.add_positional_meta("A", "a", [&](const std::string& v){ a=v; }, false);
    REQUIRE_THROWS_AS(p.parse({"one", "two", "--", "command"}), std::invalid_argument);
}

TEST_CASE("OptionParser: extra positionals with -- mismatch positional count error") {
    OptionParser p; p.allow_trailing_args(); std::string a,b; p.add_positional_meta("A","a",[&](const std::string& v){ a=v; }); p.add_positional_meta("B","b",[&](const std::string& v){ b=v; });
    // Provide fewer than required before --
    REQUIRE_THROWS_AS(p.parse({"onlyA","--","x"}), std::invalid_argument);
}

TEST_CASE("OptionParser: forbidden trailing with explicit -- errors") {
    OptionParser p; std::string a; p.add_positional_meta("A","a",[&](const std::string& v){ a=v; });
    REQUIRE_THROWS_AS(p.parse({"one","--","two"}), std::invalid_argument);
}
