#include "doctest.h"
#include "actions/enter_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace actions;

// This integration-level doctest instantiates a real EnterConfig to ensure
// parser configuration (including allow_trailing_args) works end-to-end.
// We purposefully avoid calling validate/analyze to keep dependencies light.

TEST_CASE("EnterConfig integration: ROOT plus trailing command via --") {
    EnterConfig cfg;
    ToBeParsedArgs tpa;
    tpa.args = {"/rootfs", "--", "/bin/echo", "hello"};
    // parse() should not throw and should populate shell vector via postParse
    // Expect parse to succeed (no exception)
    cfg.parse(tpa);
    CHECK(cfg.root == "/rootfs");
    REQUIRE(cfg.shell.size() == 2);
    CHECK(cfg.shell[0] == "/bin/echo");
    CHECK(cfg.shell[1] == "hello");
    REQUIRE(cfg.envVars.size() == 1);
    CHECK(cfg.envVars[0].first == "PATH");
    CHECK(cfg.envVars[0].second ==
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
}

TEST_CASE("EnterConfig environment precedence is explicit, persisted, default") {
    EnterConfig persisted;
    ToBeParsedArgs persistedArgs;
    persistedArgs.args = {"/rootfs", "--persist-env", "PATH", "--", "/bin/true"};
    persisted.parse(persistedArgs);
    REQUIRE(persisted.envVars.size() == 1);
    CHECK(persisted.envVars[0].first == "PATH");
    CHECK(persisted.envVars[0].second == ::getenv("PATH"));

    EnterConfig explicitValue;
    ToBeParsedArgs explicitArgs;
    explicitArgs.args = {"/rootfs", "--persist-env", "PATH", "--env", "PATH=/target/path",
                         "--", "/bin/true"};
    explicitValue.parse(explicitArgs);
    REQUIRE(explicitValue.envVars.size() == 1);
    CHECK(explicitValue.envVars[0].first == "PATH");
    CHECK(explicitValue.envVars[0].second == "/target/path");

    EnterConfig empty;
    ToBeParsedArgs emptyArgs;
    emptyArgs.args = {"/rootfs", "--no-default-env", "--", "/bin/true"};
    empty.parse(emptyArgs);
    CHECK(empty.envVars.empty());
}

TEST_CASE("EnterConfig integration: implicit trailing without -- is collected when allowed") {
    EnterConfig cfg;
    ToBeParsedArgs tpa;
    tpa.args = {"/rootfs", "/bin/echo", "hi"};
    // Because EnterConfig opts into trailing args, extras after ROOT become trailing
    // Expect parse to succeed (no exception)
    cfg.parse(tpa);
    CHECK(cfg.root == "/rootfs");
    REQUIRE(cfg.shell.size() == 2);
    CHECK(cfg.shell[0] == "/bin/echo");
    CHECK(cfg.shell[1] == "hi");
}

TEST_CASE("SingleConfig accepts a command without ROOT") {
    SingleConfig cfg;
    ToBeParsedArgs tpa; tpa.args = {"--", "/bin/echo"};
    cfg.parse(tpa);
    cfg.validate();
    CHECK(cfg.singleId);
    CHECK(cfg.hostVisible);
    CHECK(cfg.root.empty());
    REQUIRE(cfg.shell.size() == 1);
    CHECK(cfg.shell[0] == "/bin/echo");
}

TEST_CASE("EnterConfig accepts read-only same-path and explicit mappings") {
    EnterConfig cfg;
    ToBeParsedArgs args;
    args.args = {"/rootfs", "--map-ro", "/sys", "--map-ro",
                 "/source:/destination", "--", "/bin/true"};
    cfg.parse(args);

    REQUIRE(cfg.maps.size() == 2);
    CHECK(cfg.maps[0].src == "/sys");
    CHECK(cfg.maps[0].dst == "/sys");
    CHECK(cfg.maps[0].readonly);
    CHECK(cfg.maps[1].src == "/source");
    CHECK(cfg.maps[1].dst == "/destination");
    CHECK(cfg.maps[1].readonly);
}

TEST_CASE("EnterConfig integration: ROOT is required") {
    EnterConfig cfg;
    ToBeParsedArgs tpa; tpa.args = {"--", "/bin/echo"};
    REQUIRE_THROWS_AS(cfg.parse(tpa), std::invalid_argument);
}

TEST_CASE("EnterConfig accepts explicit foreign execution controls") {
    EnterConfig cfg;
    ToBeParsedArgs tpa;
    tpa.args = {"/", "--qemu", "/usr/bin/qemu-aarch64-static",
                "--qemu-cpu", "max,sve=on,sve256=on", "--", "/bin/sh"};
    cfg.parse(tpa);
    cfg.validate();
    CHECK(cfg.emulationPolicy == emulation::Policy::Auto);
    CHECK(cfg.qemu == "/usr/bin/qemu-aarch64-static");
    CHECK(cfg.qemuCpu == "max,sve=on,sve256=on");
}

TEST_CASE("EnterConfig validates the emulation policy") {
    EnterConfig cfg;
    ToBeParsedArgs invalid;
    invalid.args = {"/", "--emulation", "sometimes", "--", "/bin/sh"};
    REQUIRE_THROWS_AS(cfg.parse(invalid), AppException);

    EnterConfig disabled;
    ToBeParsedArgs never;
    never.args = {"/", "--emulation", "never", "--", "/bin/sh"};
    disabled.parse(never);
    disabled.validate();
    CHECK(disabled.emulationPolicy == emulation::Policy::Never);
}

TEST_CASE("EnterConfig rejects contradictory emulation controls") {
    EnterConfig cfg;
    ToBeParsedArgs tpa;
    tpa.args = {"/", "--emulation", "never", "--qemu-cpu", "qemu64",
                "--", "/bin/sh"};
    cfg.parse(tpa);
    REQUIRE_THROWS_AS(cfg.validate(), AppException);
}

TEST_CASE("SingleConfig keeps rootfs controls out of single mode") {
    for (const auto& option : std::vector<std::vector<std::string>>{
             {"--emulation", "auto"}, {"--emulation", "never"},
             {"--qemu", "/tmp/qemu"},
             {"--qemu-cpu", "qemu64"}}) {
        SingleConfig cfg;
        ToBeParsedArgs tpa;
        tpa.args = {};
        tpa.args.insert(tpa.args.end(), option.begin(), option.end());
        tpa.args.insert(tpa.args.end(), {"--", "/bin/true"});
        REQUIRE_THROWS_AS(cfg.parse(tpa), std::invalid_argument);
    }
}

TEST_CASE("EnterConfig probes the requested absolute command before /bin/sh") {
    auto root = std::filesystem::temp_directory_path() /
        ("unroot-command-probe-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(root / "bin");
    std::array<unsigned char, 64> header{};
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;
    header[5] = 1;
    header[7] = 3;
    header[18] = 183;
    {
        std::ofstream out(root / "bin/probe", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
    }

    EnterConfig cfg;
    cfg.root = root.string();
    cfg.shell = {"/bin/probe"};
    cfg.analyzeArchitecture();
    CHECK(cfg.targetArch == "arm64");
    std::filesystem::remove_all(root);
}

TEST_CASE("EnterConfig rejects recognized but unsupported ELF combinations") {
    auto root = std::filesystem::temp_directory_path() /
        ("unroot-unsupported-probe-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(root / "bin");
    std::array<unsigned char, 64> header{};
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;
    header[5] = 2;
    header[18] = 0;
    header[19] = 62;
    {
        std::ofstream out(root / "bin/probe", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
    }

    EnterConfig cfg;
    cfg.root = root.string();
    cfg.shell = {"/bin/probe"};
    bool rejected = false;
    try {
        cfg.analyzeArchitecture();
    } catch (const AppException& error) {
        rejected = true;
        CHECK(std::string(error.what()).find("unsupported ELF target") !=
              std::string::npos);
    }
    CHECK(rejected);
    std::filesystem::remove_all(root);
}

TEST_CASE("EnterConfig resolves bare and relative commands before probing ELF") {
    auto root = std::filesystem::temp_directory_path() /
        ("unroot-resolved-probe-" +
         std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(root / "bin");
    std::filesystem::create_directories(root / "work");
    std::array<unsigned char, 64> header{};
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2;
    header[5] = 1;
    header[18] = 183;
    for (const auto& path : {root / "bin" / "probe", root / "work" / "probe"}) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::add);
    }

    EnterConfig bare;
    bare.root = root.string();
    bare.shell = {"probe"};
    bare.envVars = {{"PATH", "/bin"}};
    bare.analyzeArchitecture();
    CHECK(bare.targetExecutable == "/bin/probe");
    CHECK(bare.targetArch == "arm64");

    EnterConfig relative;
    relative.root = root.string();
    relative.cwdInRoot = "/work";
    relative.shell = {"./probe"};
    relative.analyzeArchitecture();
    CHECK(relative.targetExecutable == "/work/probe");
    CHECK(relative.targetArch == "arm64");
    std::filesystem::remove_all(root);
}
