#include "doctest.h"
#include "binfmt.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

class BinfmtTree {
public:
    BinfmtTree() {
        char pattern[] = "/tmp/unroot-binfmt-XXXXXX";
        char* created = ::mkdtemp(pattern);
        REQUIRE(created != nullptr);
        root = created;
        std::ofstream(root / "register");
    }

    ~BinfmtTree() { std::filesystem::remove_all(root); }

    void handler(const std::string& name, const std::string& magic,
                 const std::string& mask, bool enabled = true,
                 const std::string& flags = "FOC") {
        std::ofstream output(root / name);
        output << (enabled ? "enabled\n" : "disabled\n")
               << "interpreter /usr/bin/qemu-test-static\n"
               << "flags: " << flags << "\n"
               << "offset 0\n"
               << "magic " << magic << "\n"
               << "mask " << mask << "\n";
    }

    std::filesystem::path root;
};

} // namespace

TEST_CASE("host binfmt registration uses fixed interpreter semantics") {
    CHECK(binfmtRegistration("unroot-arm64", "/usr/bin/qemu-aarch64-static",
                             "7f45", "ffff", "F") ==
          ":unroot-arm64:M::\\x7f\\x45:\\xff\\xff:"
          "/usr/bin/qemu-aarch64-static:F");
}

TEST_CASE("host binfmt reuses any enabled compatible handler") {
    BinfmtTree tree;
    tree.handler("qemu-aarch64", "7f454c46", "ffffffff");

    auto result = ensureHostBinfmt("unroot-arm64", {}, "7F454C46",
                                   "FFFFFFFF", tree.root);

    REQUIRE(result);
    CHECK(result.reused);
    CHECK(result.handler == "qemu-aarch64");
}

TEST_CASE("host binfmt rejects a matching handler without fixed semantics") {
    BinfmtTree tree;
    tree.handler("qemu-aarch64", "7f454c46", "ffffffff", true, "OC");

    auto result = ensureHostBinfmt("unroot-arm64", {}, "7f454c46",
                                   "ffffffff", tree.root);

    CHECK_FALSE(result);
    CHECK(result.error.find("missing F flag") != std::string::npos);
}

TEST_CASE("host binfmt prefers a fixed handler over another matching handler") {
    BinfmtTree tree;
    tree.handler("qemu-old", "7f454c46", "ffffffff", true, "OC");
    tree.handler("qemu-fixed", "7f454c46", "ffffffff", true, "F");

    auto result = ensureHostBinfmt("unroot-arm64", {}, "7f454c46",
                                   "ffffffff", tree.root);

    REQUIRE(result);
    CHECK(result.reused);
    CHECK(result.handler == "qemu-fixed");
}

TEST_CASE("host binfmt refuses to replace a conflicting Unroot handler") {
    BinfmtTree tree;
    tree.handler("unroot-arm64", "0102", "ffff");

    auto result = ensureHostBinfmt("unroot-arm64",
                                   "/usr/bin/qemu-aarch64-static",
                                   "7f45", "ffff", tree.root);

    CHECK_FALSE(result);
    CHECK(result.error.find("incompatible") != std::string::npos);
}

TEST_CASE("host binfmt writes a new handler through the register endpoint") {
    BinfmtTree tree;

    auto result = ensureHostBinfmt("unroot-arm64",
                                   "/usr/bin/qemu-aarch64-static",
                                   "7f45", "ffff", tree.root);

    REQUIRE(result);
    CHECK_FALSE(result.reused);
    std::ifstream input(tree.root / "register");
    std::string registration;
    std::getline(input, registration);
    CHECK(registration ==
          ":unroot-arm64:M::\\x7f\\x45:\\xff\\xff:"
          "/usr/bin/qemu-aarch64-static:F");
}
