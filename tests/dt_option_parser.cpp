// doctest port of OptionParser tests
#include "../third_party/doctest/doctest.h"
#include "actions/config_base.hpp" // provides full Option definition

using namespace actions;

TEST_CASE("OptionParser flag and single option parsing") {
    OptionParser p;
    bool flagA=false; std::string mode; std::vector<std::string> maps; 
    p.add_flag_meta({"--alpha","-a"}, "alpha flag", [&]{ flagA=true; });
    p.add_option_meta({"--mode","-m"}, "<val>", "mode value", [&](const std::string& v){ mode=v; });
    p.add_multi_option_meta({"--map"}, "<p>", "maps", [&](const std::string& v){ maps.push_back(v); });
    auto r = p.parse({"-a","-m755","--map","/x","--map","/y"});
    REQUIRE(flagA);
    REQUIRE(mode=="755");
    REQUIRE(maps.size()==2);
    REQUIRE(r.positionals.empty());
}

TEST_CASE("OptionParser equals format parsing") {
    OptionParser p;
    bool flagA=false; std::string mode, owner; std::vector<std::string> maps; 
    p.add_flag_meta({"--alpha","-a"}, "alpha flag", [&]{ flagA=true; });
    p.add_option_meta({"--mode"}, "<val>", "mode value", [&](const std::string& v){ mode=v; });
    p.add_option_meta({"--owner"}, "<spec>", "owner spec", [&](const std::string& v){ owner=v; });
    p.add_multi_option_meta({"--map"}, "<p>", "maps", [&](const std::string& v){ maps.push_back(v); });
    auto r = p.parse({"--mode=755", "--owner=user:group", "--map=/x", "--map=/y", "-a"});
    REQUIRE(flagA);
    REQUIRE(mode=="755");
    REQUIRE(owner=="user:group");
    REQUIRE(maps.size()==2);
    REQUIRE(maps[0]=="/x");
    REQUIRE(maps[1]=="/y");
    REQUIRE(r.positionals.empty());
}

TEST_CASE("OptionParser equals format with equals in value") {
    OptionParser p; std::string env_var;
    p.add_option_meta({"--env"}, "<var>", "environment variable", [&](const std::string& v){ env_var=v; });
    auto r = p.parse({"--env=KEY=VALUE=/path/with=sign"});
    REQUIRE(env_var=="KEY=VALUE=/path/with=sign");
    REQUIRE(r.positionals.empty());
}

TEST_CASE("OptionParser equals format error for flags") {
    OptionParser p; bool flagA=false; p.add_flag_meta({"--alpha"}, "alpha flag", [&]{ flagA=true; });
    REQUIRE_THROWS_AS(p.parse({"--alpha=true"}), std::invalid_argument);
}

TEST_CASE("OptionParser mixed formats compatibility") {
    OptionParser p; bool flagA=false; std::string mode, owner; std::vector<std::string> maps; 
    p.add_flag_meta({"--alpha","-a"}, "alpha flag", [&]{ flagA=true; });
    p.add_option_meta({"--mode"}, "<val>", "mode value", [&](const std::string& v){ mode=v; });
    p.add_option_meta({"--owner"}, "<spec>", "owner spec", [&](const std::string& v){ owner=v; });
    p.add_multi_option_meta({"--map"}, "<p>", "maps", [&](const std::string& v){ maps.push_back(v); });
    auto r = p.parse({"--mode=755", "--owner", "user:group", "--map=/x", "--map", "/y", "-a"});
    REQUIRE(flagA);
    REQUIRE(mode=="755");
    REQUIRE(owner=="user:group");
    REQUIRE(maps.size()==2);
    REQUIRE(maps[0]=="/x");
    REQUIRE(maps[1]=="/y");
    REQUIRE(r.positionals.empty());
}

TEST_CASE("OptionParser short cluster flags and value consumption") {
    OptionParser p; bool pflag=false, rflag=false; std::string val;
    p.add_flag_meta({"-p"}, "parents", [&]{ pflag=true; });
    p.add_flag_meta({"-r"}, "recursive", [&]{ rflag=true; });
    p.add_option_meta({"-m"}, "<mode>", "mode", [&](const std::string& v){ val=v; });
    auto r = p.parse({"-prm644"});
    REQUIRE(pflag);
    REQUIRE(rflag);
    REQUIRE(val=="644");
    REQUIRE(r.positionals.empty());
}

TEST_CASE("OptionParser cluster missing value error") {
    OptionParser p; bool called=false; std::string v;
    p.add_flag_meta({"-p"}, "parents", [&]{ called=true; });
    p.add_option_meta({"-m"}, "<mode>", "mode", [&](const std::string& mv){ v=mv; });
    REQUIRE_THROWS_AS(p.parse({"-pm"}), std::invalid_argument);
}

TEST_CASE("OptionParser metadata export combines names") {
    OptionParser p; p.add_flag_meta({"--parents","-p"}, "parents", []{});
    auto opts = p.exportOptions();
    REQUIRE(opts.size()==1);
    REQUIRE(opts[0].name.find("--parents")!=std::string::npos);
    REQUIRE(opts[0].name.find("-p")!=std::string::npos);
}

// (EnterConfig parsing test omitted in doctest migration for now to avoid
// pulling in large dependency graph; will be re-added separately.)
