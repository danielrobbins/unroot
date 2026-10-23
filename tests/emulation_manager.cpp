#include "doctest.h"
#include "app_exception.hpp"
#include "binfmt.hpp"
#include "emulation.hpp"
#include "meta.hpp"

// Link the real Manager against controlled external services. No test touches
// the host's binfmt_misc registry or requires an installed QEMU executable.
namespace {
HostBinfmtResult registration;
int registrations = 0;
int wrapperInstalls = 0;
bool validMagic = true;

emulation::Settings settings() {
    registration = {true, "distribution-qemu", {}};
    registrations = wrapperInstalls = 0;
    validMagic = true;
    emulation::Settings result;
    result.native = true;
    result.root = "/test-root";
    result.executable = "/bin/sh";
    result.hostArch = "x86-64bit";
    result.targetArch = "arm64";
    return result;
}

std::string failure(emulation::Settings value) {
    try {
        emulation::Manager(std::move(value)).prepare();
    } catch (const AppException& error) {
        return error.what();
    }
    throw std::runtime_error("expected emulation preparation to fail");
}
}

std::string qemuUserEmulatorForArch(const std::string&) {
    return "unroot-test-missing-qemu";
}
bool isStaticElf(const std::string&) { return false; }
ElfInfo readElfInfoAtPath(const std::string&) { return {}; }
std::pair<std::string, std::string> readElfMagicAndMaskAtRoot(
    const std::string& root, const std::string& executable) {
    REQUIRE(root == "/test-root");
    REQUIRE(executable == "/bin/sh");
    return validMagic ? std::make_pair("7f454c46", "ffffffff")
                      : std::make_pair("", "");
}
HostBinfmtResult ensureHostBinfmt(
    const std::string& name, const std::string& emulator,
    const std::string& magic, const std::string& mask,
    const std::filesystem::path&) {
    ++registrations;
    REQUIRE(name == "unroot-arm64");
    REQUIRE(emulator.empty());
    REQUIRE(magic == "7f454c46");
    REQUIRE(mask == "ffffffff");
    return registration;
}
std::string binfmtRegistration(const std::string&, const std::string&,
                              const std::string&, const std::string&,
                              const std::string&) { return {}; }
bool installSelfAsWrapper(const std::string&) {
    ++wrapperInstalls;
    return false;
}
namespace compat {
void earlyCheck(const std::string&) {}
}
namespace meta {
bool updateQemuWrapper(const fs::path&, const std::string&,
                       const std::vector<std::string>&, const std::string&) {
    return false;
}
}

TEST_CASE("native preparation reuses a handler without a local emulator") {
    auto plan = emulation::Manager(settings()).prepare();
    CHECK(registrations == 1);
    CHECK(wrapperInstalls == 0);
    CHECK(plan.source.empty());
    CHECK(plan.registration.empty());
}

TEST_CASE("native preparation reports why the existing handler cannot be reused") {
    for (const char* detail : {"missing F flag", "incompatible settings",
                              "no compatible host binfmt_misc handler is registered"}) {
        auto value = settings();
        registration = {false, {}, detail};
        auto error = failure(value);
        CHECK(error.find(detail) != std::string::npos);
        CHECK(error.find("static QEMU user emulator") != std::string::npos);
        CHECK(registrations == 1);
        CHECK(wrapperInstalls == 0);
    }
}

TEST_CASE("explicit invalid QEMU does not silently reuse host emulation") {
    auto value = settings();
    value.qemu = "/unroot-test-missing-qemu";
    CHECK(failure(value).find("trusted root-controlled path") != std::string::npos);
    CHECK(registrations == 0);
}

TEST_CASE("rootless preparation never falls back to host emulation") {
    auto value = settings();
    value.native = false;
    CHECK(failure(value).find("static QEMU user emulator") != std::string::npos);
    CHECK(registrations == 0);
}

TEST_CASE("native handler reuse respects emulation policy and CPU restrictions") {
    auto value = settings();
    value.policy = emulation::Policy::Never;
    CHECK(failure(value).find("disabled by --emulation=never") != std::string::npos);
    CHECK(registrations == 0);
    value = settings();
    value.cpu = "cortex-a53";
    CHECK(failure(value).find("--qemu-cpu is unavailable") != std::string::npos);
    CHECK(registrations == 0);
}

TEST_CASE("native handler reuse still requires the target ELF format") {
    auto value = settings();
    validMagic = false;
    CHECK(failure(value).find("cannot determine target ELF format") != std::string::npos);
    CHECK(registrations == 0);
}
