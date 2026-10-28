#include "doctest.h"
#include "test_support.hpp"
#include "actions/enter_config.hpp"
#include "actions/parsed_args.hpp"
#include "app_exception.hpp"
#include "arch.hpp"
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
    auto path = std::find_if(cfg.envVars.begin(), cfg.envVars.end(),
        [](const auto& value) { return value.first == "PATH"; });
    REQUIRE(path != cfg.envVars.end());
    CHECK(path->second ==
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
}
TEST_CASE("EnterConfig environment precedence is explicit, persisted, default") {
    EnterConfig persisted;
    ToBeParsedArgs persistedArgs;
    persistedArgs.args = {"/rootfs", "--persist-env", "PATH", "--", "/bin/true"};
    persisted.parse(persistedArgs);
    auto persistedPath = std::find_if(persisted.envVars.begin(), persisted.envVars.end(),
        [](const auto& value) { return value.first == "PATH"; });
    REQUIRE(persistedPath != persisted.envVars.end());
    CHECK(persistedPath->second == ::getenv("PATH"));

    EnterConfig explicitValue;
    ToBeParsedArgs explicitArgs;
    explicitArgs.args = {"/rootfs", "--persist-env", "PATH", "--env", "PATH=/target/path",
                         "--", "/bin/true"};
    explicitValue.parse(explicitArgs);
    auto explicitPath = std::find_if(explicitValue.envVars.begin(), explicitValue.envVars.end(),
        [](const auto& value) { return value.first == "PATH"; });
    REQUIRE(explicitPath != explicitValue.envVars.end());
    CHECK(explicitPath->second == "/target/path");

    EnterConfig empty;
    ToBeParsedArgs emptyArgs;
    emptyArgs.args = {"/rootfs", "--no-default-env", "--", "/bin/true"};
    empty.parse(emptyArgs);
    REQUIRE(empty.envVars.size() <= 1);
    if (!empty.envVars.empty()) {
        CHECK(empty.envVars[0].first == "TERM");
        CHECK(empty.envVars[0].second == ::getenv("TERM"));
    }
}

TEST_CASE("EnterConfig automatically preserves TERM when available") {
    const char* term = ::getenv("TERM");
    EnterConfig cfg;
    ToBeParsedArgs args;
    args.args = {"/rootfs", "--", "/bin/true"};
    cfg.parse(args);

    auto item = std::find_if(cfg.envVars.begin(), cfg.envVars.end(),
        [](const auto& value) { return value.first == "TERM"; });
    if (term && *term) {
        REQUIRE(item != cfg.envVars.end());
        CHECK(item->second == term);
    } else {
        CHECK(item == cfg.envVars.end());
    }
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
    tpa.args = {"/tmp", "--qemu", "/usr/bin/qemu-aarch64-static",
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
    never.args = {"/tmp", "--emulation", "never", "--", "/bin/sh"};
    disabled.parse(never);
    disabled.validate();
    CHECK(disabled.emulationPolicy == emulation::Policy::Never);
}

TEST_CASE("EnterConfig rejects contradictory emulation controls") {
    EnterConfig cfg;
    ToBeParsedArgs tpa;
    tpa.args = {"/tmp", "--emulation", "never", "--qemu-cpu", "qemu64",
                "--", "/bin/sh"};
    cfg.parse(tpa);
    REQUIRE_THROWS_AS(cfg.validate(), AppException);
}

TEST_CASE("EnterConfig probes the requested absolute command before /bin/sh") {
    test::TempDirectory fixture;
    const auto& root = fixture.root;
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
}

TEST_CASE("EnterConfig rejects recognized but unsupported ELF combinations") {
    test::TempDirectory fixture;
    const auto& root = fixture.root;
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
}

TEST_CASE("EnterConfig resolves bare and relative commands before probing ELF") {
    test::TempDirectory fixture;
    const auto& root = fixture.root;
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
}

TEST_CASE("EnterConfig can prepare foreign children without changing the command") {
    test::TempDirectory fixture;
    const auto& root = fixture.root;
    std::filesystem::create_directories(root / "bin");
    std::filesystem::copy_file("/proc/self/exe", root / "bin/native",
        std::filesystem::copy_options::overwrite_existing);
    const std::string host = detectHostArch();
    const int machine = host == "arm64" ? 62 : 183;
    const std::string foreign = host == "arm64" ? "x86_64" : "arm64";
    std::array<unsigned char, 64> header{};
    header[0] = 0x7f; header[1] = 'E'; header[2] = 'L'; header[3] = 'F';
    header[4] = 2; header[5] = 1; header[18] = machine;
    {
        std::ofstream out(root / "bin/foreign", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
    }
    {
        std::ofstream out(root / "bin/script");
        out << "#!/bin/foreign\n";
    }
    std::filesystem::create_symlink("/bin/foreign", root / "bin/link");

    EnterConfig cfg;
    cfg.root = root.string();
    cfg.shell = {"/bin/native", "unchanged"};
    cfg.analyzeArchitecture();
    CHECK_FALSE(cfg.isCrossArch);
    for (const auto& reference : {"/bin/foreign", "/bin/script", "/bin/link"}) {
        cfg.emulationFor = reference;
        cfg.analyzeArchitecture();
        CHECK(cfg.isCrossArch);
        CHECK(cfg.targetArch == foreign);
        CHECK((cfg.targetExecutable == "/bin/foreign" || cfg.targetExecutable == "/bin/link"));
        CHECK((cfg.shell == std::vector<std::string>{"/bin/native", "unchanged"}));
    }
    cfg.shell = {"/bin/foreign"};
    cfg.emulationFor = "/bin/native";
    cfg.analyzeArchitecture();
    CHECK(cfg.isCrossArch);
    CHECK(cfg.targetExecutable == "/bin/foreign");
    cfg.emulationFor = "/bin/foreign";
    cfg.analyzeArchitecture();
    cfg.emulationFor = "/bin/missing";
    REQUIRE_THROWS_AS(cfg.analyzeArchitecture(), AppException);

    header[18] = host == "riscv64" ? 62 : 243;
    {
        std::ofstream out(root / "bin/other", std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), header.size());
    }
    cfg.emulationFor = "/bin/other";
    REQUIRE_THROWS_AS(cfg.analyzeArchitecture(), AppException);
}

TEST_CASE("EnterConfig requires an absolute emulation reference") {
    EnterConfig cfg;
    ToBeParsedArgs args;
    args.args = {"/rootfs", "--emulation-for", "bin/sh", "--", "/bin/native"};
    REQUIRE_THROWS_AS(cfg.parse(args), AppException);
    args.args[2] = "/bin/sh";
    cfg.parse(args);
    CHECK(cfg.emulationFor == "/bin/sh");
}

namespace {

struct MappedRoots {
    test::TempDirectory fixture;
    std::filesystem::path base = fixture.root;
    std::filesystem::path root = base / "roots" / "arch";
    std::filesystem::path sibling = base / "roots" / "other";
    std::filesystem::path unrelated = base / "elsewhere";

    MappedRoots() {
        for (const auto& path : {root, sibling, unrelated})
            std::filesystem::create_directories(path);
    }

    static EnterConfig parse(const std::filesystem::path& root, const std::string& option,
                             const std::filesystem::path& source) {
        EnterConfig cfg;
        ToBeParsedArgs args;
        args.args = {root.string(), option, source.string(), "--", "/bin/sh"};
        cfg.parse(args);
        return cfg;
    }

    // True when validation refuses the mapping of SOURCE for ROOT.
    static bool rejects(const std::filesystem::path& root, const std::string& option,
                        const std::filesystem::path& source) {
        auto cfg = parse(root, option, source);
        try {
            cfg.validate();
        } catch (const AppException&) {
            return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("EnterConfig rejects a mapping of the active rootfs or an ancestor") {
    MappedRoots tree;
    for (const std::string option : {"--map", "--map-ro"}) {
        REQUIRE(MappedRoots::rejects(tree.root, option, tree.root));
        REQUIRE(MappedRoots::rejects(tree.root, option, tree.root.parent_path()));
        REQUIRE(MappedRoots::rejects(tree.root, option, tree.base));
        REQUIRE(MappedRoots::rejects(tree.root, option, "/"));
    }
}

TEST_CASE("EnterConfig names both paths when it rejects a mapping") {
    MappedRoots tree;
    auto cfg = MappedRoots::parse(tree.root, "--map", tree.base);
    std::string message;
    try {
        cfg.validate();
    } catch (const AppException& error) {
        message = error.what();
    }
    REQUIRE(message.find(std::filesystem::canonical(tree.base).string()) != std::string::npos);
    REQUIRE(message.find(std::filesystem::canonical(tree.root).string()) != std::string::npos);
    REQUIRE(message.find("narrower") != std::string::npos);
}

TEST_CASE("EnterConfig rejects an alias of an ancestor of the rootfs") {
    MappedRoots tree;
    const auto alias = tree.unrelated / "alias";
    std::filesystem::create_directory_symlink(tree.root.parent_path(), alias);
    REQUIRE(MappedRoots::rejects(tree.root, "--map", alias));
}

TEST_CASE("EnterConfig rejects a mapping when ROOT is itself an alias") {
    MappedRoots tree;
    const auto alias = tree.unrelated / "root-alias";
    std::filesystem::create_directory_symlink(tree.root, alias);
    REQUIRE(MappedRoots::rejects(alias, "--map", tree.root.parent_path()));
    REQUIRE(MappedRoots::rejects(alias, "--map-ro", tree.root));
}

TEST_CASE("EnterConfig still allows mappings that do not contain the rootfs") {
    MappedRoots tree;
    std::filesystem::create_directories(tree.root / "mnt");
    for (const std::string option : {"--map", "--map-ro"}) {
        // A sibling root, an unrelated directory, a directory inside the
        // rootfs, and a path that does not exist yet.
        CHECK_FALSE(MappedRoots::rejects(tree.root, option, tree.sibling));
        CHECK_FALSE(MappedRoots::rejects(tree.root, option, tree.unrelated));
        CHECK_FALSE(MappedRoots::rejects(tree.root, option, tree.root / "mnt"));
        CHECK_FALSE(MappedRoots::rejects(tree.root, option, tree.base / "missing"));
    }
}
