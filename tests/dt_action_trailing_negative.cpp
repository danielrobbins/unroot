#include "doctest.h"
#include "actions/config_base.hpp"
#include "actions/parsed_args.hpp"
#include <string>

using namespace actions;

// Dummy action config that does NOT opt into trailing args; used to validate
// policy enforcement at the ActionConfig layer (not just raw OptionParser).
class DummyNoTrailingConfig : public ActionConfig {
public:
    std::string first;
    std::string getActionName() const override { return "dummy"; }
protected:
    void configure_parser() override {
        ActionConfig::configure_parser();
        parser_.add_positional_meta("FIRST", "First required positional", [&](const std::string& v){ first = v; });
        // Intentionally do NOT call allow_trailing_args(); extra args should error.
    }
};

TEST_CASE("ActionConfig negative: extra implicit positional when trailing not allowed") {
    DummyNoTrailingConfig cfg;
    ToBeParsedArgs tpa; tpa.args = {"one", "two"};
    REQUIRE_THROWS_AS(cfg.parse(tpa), std::invalid_argument);
}

TEST_CASE("ActionConfig negative: explicit -- with trailing when not allowed") {
    DummyNoTrailingConfig cfg;
    ToBeParsedArgs tpa; tpa.args = {"one", "--", "two"};
    REQUIRE_THROWS_AS(cfg.parse(tpa), std::invalid_argument);
}
