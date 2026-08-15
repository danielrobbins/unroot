#include "doctest.h"
#include "injections.hpp"

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

class InjectionTree {
 public:
  InjectionTree() {
    char pattern[] = "/tmp/unroot-injections-XXXXXX";
    char* created = ::mkdtemp(pattern);
    REQUIRE(created != nullptr);
    root = created;
    std::filesystem::create_directories(
        root / ".unroot" / "injections" / "original" / "etc");
  }

  ~InjectionTree() { std::filesystem::remove_all(root); }

  std::filesystem::path root;
};

}  // namespace

TEST_CASE("injection specs accept builtins and typed file mappings") {
  std::string error;
  injections::Spec builtin;
  REQUIRE(injections::parseSpec("hosts", builtin, error));
  CHECK(builtin.name == "hosts");
  CHECK(builtin.source == "/etc/hosts");
  CHECK(builtin.destination == "/etc/hosts");

  injections::Spec mtab;
  REQUIRE(injections::parseSpec("mtab", mtab, error));
  CHECK(mtab.name == "mtab");
  CHECK(mtab.kind == injections::Kind::Symlink);
  CHECK(mtab.target == "/proc/self/mounts");
  CHECK(mtab.destination == "/etc/mtab");
  CHECK(mtab.mode == 0777);

  injections::Spec custom;
  REQUIRE(injections::parseSpec(
      "/host/file:/etc/file:12:34:0600", custom, error));
  CHECK(custom.name.empty());
  CHECK(custom.source == "/host/file");
  CHECK(custom.destination == "/etc/file");
  CHECK(custom.uid == 12);
  CHECK(custom.gid == 34);
  CHECK(custom.mode == 0600);
}

TEST_CASE("injection specs reject malformed paths and attributes") {
  for (const char* value : {"unknown", "relative/path", "/source:relative",
                            "/source:/target:0:0:0999",
                            "/source:/target:0:0",
                            "/source:/target:4294967295:0:0600"}) {
    injections::Spec spec;
    std::string error;
    CHECK_FALSE(injections::parseSpec(value, spec, error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("unpack injection controls accept only named exclusions") {
  std::vector<std::string> disabled;
  std::string error;
  REQUIRE(injections::parseUnpackExclusions(
      "-hosts,-resolv.conf,-mtab", disabled, error));
  CHECK((disabled ==
         std::vector<std::string>{"hosts", "resolv.conf", "mtab"}));

  REQUIRE(injections::parseUnpackExclusions("-*", disabled, error));
  CHECK(disabled.back() == "*");

  for (const char* value : {"hosts", "/host/file:/etc/file", "-unknown"}) {
    std::vector<std::string> rejected;
    error.clear();
    CHECK_FALSE(injections::parseUnpackExclusions(value, rejected, error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("archive planning substitutes registered portable originals") {
  InjectionTree tree;
  std::ofstream(tree.root / ".unroot" / "injections" / "registry.json")
      << R"({"version":"unroot.injections/v1","entries":[)"
         R"({"name":"hosts","destination":"/etc/hosts",)"
         R"("original":"regular","uid":0,"gid":0,"mode":420}]})";
  std::ofstream(tree.root / ".unroot" / "injections" / "original" / "etc" /
                "hosts")
      << "portable\n";

  auto plan = injections::archivePlan(tree.root.string());
  REQUIRE(plan);
  CHECK(plan.excludes == std::vector<std::string>{"etc/hosts"});
  REQUIRE(plan.substitutions.size() == 1);
  CHECK(plan.substitutions.front().source ==
        ".unroot/injections/original/etc/hosts");
  CHECK(plan.substitutions.front().destination == "etc/hosts");
}

TEST_CASE("injection registry binds built-in names to built-in destinations") {
  InjectionTree tree;
  std::ofstream(tree.root / ".unroot" / "injections" / "registry.json")
      << R"({"version":"unroot.injections/v1","entries":[)"
         R"({"name":"hosts","destination":"/etc/not-hosts",)"
         R"("original":"absent","uid":0,"gid":0,"mode":420}]})";

  auto plan = injections::archivePlan(tree.root.string());
  CHECK_FALSE(plan);
  CHECK(plan.error == "malformed injection registry");
}
