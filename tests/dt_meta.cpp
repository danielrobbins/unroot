#include "doctest.h"
#include "meta.hpp"
#include "util/idmap.hpp"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class MetaTree {
public:
    MetaTree() {
        char pattern[] = "/tmp/unroot-meta-XXXXXX";
        char* created = ::mkdtemp(pattern);
        REQUIRE(created != nullptr);
        root = created;
    }

    ~MetaTree() { std::filesystem::remove_all(root); }

    std::filesystem::path root;
};

} // namespace

TEST_CASE("rootfs metadata persists the exact kernel ID map") {
    MetaTree tree;
    auto plan = util::makeRichIdMap(1000, 2000, 100000, 200000, 16,
                                    "files");

    auto initialized = meta::initializeIdMap(tree.root, plan);
    REQUIRE(initialized);
    CHECK(initialized.created);

    auto loaded = meta::readIdMap(tree.root);
    REQUIRE(loaded);
    CHECK(loaded.plan.mode == util::IdMapMode::Rich);
    CHECK(loaded.plan.uids == plan.uids);
    CHECK(loaded.plan.gids == plan.gids);

    auto json = meta::loadMetaJson(tree.root);
    CHECK(json["version"] == "unroot.meta/v1");
    CHECK(json["idmap"]["uid_map"][1]["outside"] == 100000);
    CHECK(json["idmap"]["gid_map"][1]["outside"] == 200000);
    CHECK_FALSE(json.contains("injections"));
}

TEST_CASE("rootfs metadata persists a larger rich ID map") {
    MetaTree tree;
    auto plan = util::makeRichIdMap(1000, 2000, 100000, 300000, 100000,
                                    "files");

    auto initialized = meta::initializeIdMap(tree.root, plan);
    REQUIRE(initialized);

    auto loaded = meta::readIdMap(tree.root);
    REQUIRE(loaded);
    CHECK(util::subordinateIdCount(loaded.plan) == 100000);
    CHECK(loaded.plan.uids == plan.uids);
    CHECK(loaded.plan.gids == plan.gids);
}

TEST_CASE("existing rootfs metadata wins initialization races") {
    MetaTree tree;
    auto first = util::makeRichIdMap(1000, 1000, 100000, 100000, 16,
                                    "files");
    auto second = util::makeRichIdMap(1000, 1000, 300000, 300000, 32,
                                     "files");

    REQUIRE(meta::initializeIdMap(tree.root, first));
    auto repeated = meta::initializeIdMap(tree.root, second);
    REQUIRE(repeated);
    CHECK_FALSE(repeated.created);
    CHECK(repeated.plan.uids == first.uids);
    CHECK(repeated.plan.gids == first.gids);
}

TEST_CASE("rootfs metadata persists native ownership without ID maps") {
    MetaTree tree;

    auto initialized = meta::initializeIdMap(tree.root, util::makeNativeIdMap());
    REQUIRE(initialized);
    auto loaded = meta::readIdMap(tree.root);
    REQUIRE(loaded);
    CHECK(loaded.plan.mode == util::IdMapMode::Native);
    CHECK(loaded.plan.uids.empty());
    CHECK(loaded.plan.gids.empty());

    auto json = meta::loadMetaJson(tree.root);
    CHECK(json["idmap"]["mode"] == "native");
    CHECK(json["idmap"]["uid_map"].empty());
    CHECK(json["idmap"]["gid_map"].empty());
}

TEST_CASE("concurrent rootfs initialization produces one complete mapping") {
    MetaTree tree;
    auto first = util::makeRichIdMap(1000, 1000, 100000, 100000, 16,
                                    "files");
    auto second = util::makeRichIdMap(1000, 1000, 300000, 300000, 32,
                                     "files");

    pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
        _exit(meta::initializeIdMap(tree.root, second) ? 0 : 1);
    auto parent = meta::initializeIdMap(tree.root, first);
    REQUIRE(parent);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    auto loaded = meta::readIdMap(tree.root);
    REQUIRE(loaded);
    const auto start = loaded.plan.uids[1].outside;
    CHECK((start == 100000 || start == 300000));
    CHECK(loaded.plan.uids[1].count == loaded.plan.gids[1].count);
}

TEST_CASE("malformed or obsolete metadata is never replaced silently") {
    MetaTree tree;
    std::filesystem::create_directory(tree.root / ".unroot");
    std::ofstream(tree.root / ".unroot" / "meta.json")
        << "{\"version\":\"unroot.meta/v0\",\"idmap\":\"single\"}\n";

    auto loaded = meta::readIdMap(tree.root);
    CHECK_FALSE(loaded);
    CHECK(loaded.error.find("unsupported") != std::string::npos);

    auto initialized = meta::initializeIdMap(
        tree.root,
        util::makeRichIdMap(1000, 1000, 100000, 100000, 16, "files"));
    CHECK_FALSE(initialized);
    CHECK(initialized.error.find("unsupported") != std::string::npos);
}

TEST_CASE("single-ID mappings cannot become managed rootfs metadata") {
    MetaTree tree;

    auto initialized = meta::initializeIdMap(
        tree.root, util::makeSingleIdMap(1000, 1000));

    CHECK_FALSE(initialized);
    CHECK(initialized.error.find("single-ID") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tree.root / ".unroot"));
}

TEST_CASE("emulator settings alone do not record rootfs ownership") {
    MetaTree tree;
    std::filesystem::create_directory(tree.root / ".unroot");
    REQUIRE(meta::updateQemuWrapper(tree.root, "/.unroot/bin/qemu-aarch64",
                                   {}, "test"));

    auto loaded = meta::readIdMap(tree.root);
    CHECK_FALSE(loaded.found);
    CHECK(loaded.error.empty());
    CHECK(meta::loadMetaJson(tree.root).contains("qemuWrapper"));
}

TEST_CASE("emulator settings do not hide malformed ownership metadata") {
    MetaTree tree;
    std::filesystem::create_directory(tree.root / ".unroot");
    REQUIRE(meta::updateQemuWrapper(tree.root, "/.unroot/bin/qemu-aarch64",
                                   {}, "test"));
    auto value = meta::loadMetaJson(tree.root);
    value["idmap"] = nullptr;
    REQUIRE(meta::atomicSaveMetaJson(tree.root, value));

    auto loaded = meta::readIdMap(tree.root);
    CHECK(loaded.found);
    CHECK_FALSE(loaded.error.empty());
}

TEST_CASE("managed rootfs metadata rejects the retired single mode") {
    MetaTree tree;
    std::filesystem::create_directory(tree.root / ".unroot");
    std::ofstream(tree.root / ".unroot" / "meta.json")
        << R"({"version":"unroot.meta/v1","idmap":{"mode":"user",)"
           R"("source":"process","uid_map":[{"inside":0,"outside":1000,)"
           R"("count":1}],"gid_map":[{"inside":0,"outside":1000,"count":1}]}})";

    auto loaded = meta::readIdMap(tree.root);

    CHECK_FALSE(loaded);
    CHECK(loaded.error.find("unknown ID map mode") != std::string::npos);
}

TEST_CASE("rootfs metadata resolves a pinned prefixed layout") {
    MetaTree tree;
    auto plan = util::makeRichIdMap(1000, 1000, 100000, 100000, 16,
                                    "files");
    REQUIRE(meta::initializeIdMap(tree.root, plan));
    std::filesystem::create_directories(tree.root / "bundle" / "rootfs");
    std::ofstream(tree.root / "bundle" / "rootfs" / "payload") << "ok\n";

    std::string error;
    REQUIRE(meta::setRootPrefix(tree.root, "bundle/rootfs", error));
    auto resolved = meta::resolveRootLayout(tree.root);

    REQUIRE(resolved);
    CHECK(resolved.layout.managedRoot == tree.root);
    CHECK(resolved.layout.rootPrefix == "bundle/rootfs");
    CHECK(resolved.layout.prefixed());
    CHECK(resolved.layout.inManagedRoot("/etc/hosts") ==
          "/bundle/rootfs/etc/hosts");
    CHECK(std::filesystem::exists(resolved.layout.effectiveRoot / "payload"));
    CHECK(meta::loadMetaJson(tree.root)["layout"]["root_prefix"] ==
          "bundle/rootfs");
}

TEST_CASE("rootfs metadata rejects missing and symlinked root prefixes") {
    MetaTree tree;
    auto plan = util::makeRichIdMap(1000, 1000, 100000, 100000, 16,
                                    "files");
    REQUIRE(meta::initializeIdMap(tree.root, plan));

    std::string error;
    CHECK_FALSE(meta::setRootPrefix(tree.root, "missing", error));
    std::filesystem::create_directory(tree.root / "real");
    std::filesystem::create_directory_symlink("real", tree.root / "link");
    error.clear();
    CHECK_FALSE(meta::setRootPrefix(tree.root, "link", error));
}

TEST_CASE("rootfs metadata records OCI provenance and a durable identity") {
    MetaTree tree;
    REQUIRE(meta::initializeIdMap(tree.root, util::makeNativeIdMap()));
    const nlohmann::json provenance{
        {"mode", "flattened"}, {"manifest_digest", "sha256:1234"}};

    std::string error;
    REQUIRE(meta::setOciProvenance(tree.root, provenance, error));
    const auto first = meta::loadMetaJson(tree.root);
    REQUIRE(first.contains("root_id"));
    REQUIRE(first["root_id"].is_string());
    CHECK(std::regex_match(first["root_id"].get<std::string>(),
                           std::regex("[0-9a-f]{32}")));
    CHECK(first["oci"] == provenance);

    const std::string identity = first["root_id"].get<std::string>();
    REQUIRE(meta::setOciProvenance(
        tree.root, {{"mode", "flattened"}, {"platform", "linux/amd64"}},
        error));
    CHECK(meta::loadMetaJson(tree.root)["root_id"] == identity);
}
