#include <ctime>
#include <climits>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hostcaps.hpp"
#include "nlohmann/json.hpp"
#include "linuxns.hpp"
#include "meta.hpp"
#include "util/rootfs.hpp"
#include "util/subid.hpp"

namespace meta {
namespace fs = std::filesystem;

static constexpr const char* MetaVersion = "unroot.meta/v1";
static constexpr const char* MetaDir = "/.unroot";
static constexpr const char* MetaPath = "/.unroot/meta.json";
static constexpr const char* MetaLock = "/.unroot/meta.lock";

struct MetaPaths { std::string env; std::string rootfs; };
struct MetaObj {
  std::string version;
  std::string createdAt;
  std::string mode;
  util::IdMapPlan idmap;
  MetaPaths paths;
  std::vector<std::string> features;
};

static nlohmann::json extentToJson(const util::IdMapExtent& extent) {
  return {{"inside", extent.inside}, {"outside", extent.outside},
          {"count", extent.count}};
}

static nlohmann::json idMapToJson(const util::IdMapPlan& plan) {
  nlohmann::json uids = nlohmann::json::array();
  nlohmann::json gids = nlohmann::json::array();
  for (const auto& extent : plan.uids) uids.push_back(extentToJson(extent));
  for (const auto& extent : plan.gids) gids.push_back(extentToJson(extent));
  const char* mode = plan.mode == util::IdMapMode::Rich ? "rich" : "native";
  return {{"mode", mode},
          {"source", plan.source}, {"uid_map", std::move(uids)},
          {"gid_map", std::move(gids)}};
}

static std::string metaToJsonString(const MetaObj& m) {
  nlohmann::json j = nlohmann::json::object();
  j["version"] = m.version;
  j["createdAt"] = m.createdAt;
  j["mode"] = m.mode;
  j["idmap"] = idMapToJson(m.idmap);
  j["paths"] = { {"env", m.paths.env}, {"rootfs", m.paths.rootfs} };
  j["features"] = m.features;
  return j.dump(2);
}

static std::string initialMeta(const fs::path& rootfs,
                               const util::IdMapPlan& idmap) {
  std::time_t now = std::time(nullptr);
  char created[32];
  std::strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ",
                std::gmtime(&now));
  MetaObj meta{MetaVersion,
               created,
               "shared-rw",
               idmap,
               {rootfs.string(), rootfs.string()},
               resolveFeatureNamesFromEnv()};
  return metaToJsonString(meta);
}

static bool parseExtent(const nlohmann::json& value,
                        util::IdMapExtent& extent) {
  if (!value.is_object()) return false;
  for (const char* key : {"inside", "outside", "count"})
    if (!value.contains(key) || !value[key].is_number_integer()) return false;
  auto inside = value["inside"].get<unsigned long long>();
  auto outside = value["outside"].get<unsigned long long>();
  auto count = value["count"].get<unsigned long long>();
  if (inside > UINT_MAX || outside > UINT_MAX || count > UINT_MAX) return false;
  extent = {static_cast<unsigned int>(inside),
            static_cast<unsigned int>(outside),
            static_cast<unsigned int>(count)};
  return true;
}

static bool parseExtents(const nlohmann::json& value,
                         std::vector<util::IdMapExtent>& extents) {
  if (!value.is_array()) return false;
  extents.clear();
  for (const auto& item : value) {
    util::IdMapExtent extent;
    if (!parseExtent(item, extent)) return false;
    extents.push_back(extent);
  }
  return true;
}

static IdMapMetaResult parseIdMapMeta(const std::string& content) {
  try {
    auto value = nlohmann::json::parse(content);
    if (!value.is_object() || !value.contains("version") ||
        value["version"] != MetaVersion || !value.contains("idmap") ||
        !value["idmap"].is_object()) {
      return {true, false, {}, "unsupported or malformed rootfs metadata"};
    }
    const auto& idmap = value["idmap"];
    if (!idmap.contains("mode") || !idmap["mode"].is_string() ||
        !idmap.contains("source") || !idmap["source"].is_string() ||
        !idmap.contains("uid_map") || !idmap.contains("gid_map")) {
      return {true, false, {}, "rootfs metadata has no valid ID map"};
    }
    util::IdMapPlan plan;
    const std::string mode = idmap["mode"].get<std::string>();
    if (mode == "rich") plan.mode = util::IdMapMode::Rich;
    else if (mode == "native") plan.mode = util::IdMapMode::Native;
    else return {true, false, {}, "rootfs metadata has an unknown ID map mode"};
    plan.source = idmap["source"].get<std::string>();
    if (!parseExtents(idmap["uid_map"], plan.uids) ||
        !parseExtents(idmap["gid_map"], plan.gids)) {
      return {true, false, {}, "rootfs metadata has malformed ID map extents"};
    }
    std::string error = util::validateIdMapPlan(plan);
    if (!error.empty()) return {true, false, {}, std::move(error)};
    return {true, false, std::move(plan), {}};
  } catch (...) {
    return {true, false, {}, "unable to parse rootfs metadata"};
  }
}

std::pair<bool, std::string> readMeta(const fs::path& env) {
  util::Rootfs root(env.string());
  std::string content;
  return root && root.readText(MetaPath, content)
             ? std::make_pair(true, std::move(content))
             : std::make_pair(false, std::string{});
}

nlohmann::json loadMetaJson(const fs::path& rootfs) {
  auto [available, content] = readMeta(rootfs);
  if (!available) return nlohmann::json::object();
  try {
    return nlohmann::json::parse(content);
  } catch (...) {
    return nlohmann::json::object();
  }
}

IdMapMetaResult readIdMap(const fs::path& rootfs) {
  auto [available, content] = readMeta(rootfs);
  return available ? parseIdMapMeta(content) : IdMapMetaResult{};
}

IdMapMetaResult initializeIdMap(const fs::path& rootfs,
                                const util::IdMapPlan& plan) {
  if (plan.mode == util::IdMapMode::Single)
    return {false, false, {},
            "single-ID mode does not initialize rootfs metadata"};
  std::string validation = util::validateIdMapPlan(plan);
  if (!validation.empty()) return {false, false, {}, std::move(validation)};
  util::Rootfs root(rootfs.string());
  if (!root || !root.directory(MetaDir, true))
    return {false, false, {}, "unable to prepare rootfs metadata directory"};
  auto lock = root.file(MetaLock, O_RDWR | O_CREAT, 0600, true);
  if (!lock || ::flock(lock.get(), LOCK_EX) != 0)
    return {false, false, {}, "unable to lock rootfs metadata"};

  std::string content;
  if (root.readText(MetaPath, content)) return parseIdMapMeta(content);
  if (errno != ENOENT)
    return {false, false, {}, "unable to read rootfs metadata"};
  content = initialMeta(rootfs, plan);
  if (!root.writeTextAtomic(MetaPath, content, 0600))
    return {false, false, {}, "unable to write rootfs metadata"};
  auto result = parseIdMapMeta(content);
  result.created = static_cast<bool>(result);
  return result;
}

IdMapMetaResult selectIdMap(util::IdMapMode mode, unsigned int richCount) {
  if (mode == util::IdMapMode::Native) {
    if (::geteuid() != 0)
      return {false, false, {}, "native ownership requires host root privileges"};
    return {true, false, util::makeNativeIdMap(), {}};
  }
  if (mode == util::IdMapMode::Single)
    return {false, false, {},
            "single-ID mode does not initialize a root filesystem"};
  if (::access("/usr/bin/newuidmap", X_OK) != 0 ||
      ::access("/usr/bin/newgidmap", X_OK) != 0)
    return {false, false, {},
            "rich ID mapping requires /usr/bin/newuidmap and "
            "/usr/bin/newgidmap; configure subordinate IDs or use "
            "'sudo unroot unpack --native'"};
  auto allocation = util::querySubIdAllocation(richCount);
  if (!allocation) {
    std::string error = std::move(allocation.error);
    if (error.rfind("error: ", 0) == 0) error.erase(0, 7);
    error += "; configure subordinate IDs or use "
             "'sudo unroot unpack --native'";
    return {false, false, {}, std::move(error)};
  }
  return {true, false,
          util::makeRichIdMap(::getuid(), ::getgid(),
                              allocation.allocation.uidStart,
                              allocation.allocation.gidStart,
                              allocation.allocation.count,
                              allocation.allocation.source), {}};
}

IdMapMetaResult resolveIdMap(const fs::path& rootfs, util::IdMapMode mode,
                             unsigned int richCount, bool specified) {
  auto stored = readIdMap(rootfs);
  if (!stored.error.empty()) return stored;
  if (!stored.found) {
    auto selected = selectIdMap(mode, richCount);
    if (!selected) return selected;
    stored = initializeIdMap(rootfs, selected.plan);
    if (!stored) return stored;
  }
  const auto& plan = stored.plan;
  if (specified && plan.mode != mode)
    return {true, false, {}, "requested ID map mode differs from initialized rootfs"};
  if (specified && plan.mode == util::IdMapMode::Rich &&
      util::subordinateIdCount(plan) != richCount)
    return {true, false, {}, "requested rich ID count differs from initialized rootfs"};
  if (plan.mode == util::IdMapMode::Native) {
    if (::geteuid() != 0)
      return {true, false, {}, "native ownership requires host root privileges"};
    return stored;
  }
  if (plan.uids.front().outside != static_cast<unsigned int>(::getuid()) ||
      plan.gids.front().outside != static_cast<unsigned int>(::getgid()))
    return {true, false, {}, "rootfs was initialized by a different host UID or GID"};
  if (plan.mode == util::IdMapMode::Rich) {
    util::SubIdAllocation allocation{plan.uids[1].outside,
                                     plan.gids[1].outside,
                                     plan.uids[1].count, plan.source};
    auto validated = util::validateSubIdAllocation(allocation);
    if (!validated) {
      std::string error = std::move(validated.error);
      if (error.rfind("error: ", 0) == 0) error.erase(0, 7);
      return {true, false, {}, std::move(error)};
    }
  }
  return stored;
}

bool atomicSaveMetaJson(const fs::path& rootfs, const nlohmann::json& j) {
  util::Rootfs root(rootfs.string());
  return root && root.writeTextAtomic(MetaPath, j.dump(2) + "\n");
}

bool ensureVersionAndSchema(nlohmann::json& j, const fs::path& rootfs,
                            int curSchema,
                            int verMajor, int verMinor, int verPatch,
                            const std::string& gitSha) {
  bool changed = false;
  if (!j.is_object()) { j = nlohmann::json::object(); changed = true; }
  // metaSchema
  if (!j.contains("metaSchema") || !j["metaSchema"].is_number_integer()) { j["metaSchema"] = curSchema; changed = true; }
  else if (j["metaSchema"].get<int>() != curSchema) { j["metaSchema"] = curSchema; changed = true; }
  // unrootVersion array
  auto needSetVersion = [&]() -> bool {
    if (!j.contains("unrootVersion") || !j["unrootVersion"].is_array()) return true;
    const auto& a = j["unrootVersion"]; if (a.size() != 3) return true;
    return !(a[0].is_number_integer() && a[1].is_number_integer() && a[2].is_number_integer() &&
             a[0].get<int>() == verMajor && a[1].get<int>() == verMinor && a[2].get<int>() == verPatch);
  }();
  if (needSetVersion) { j["unrootVersion"] = {verMajor, verMinor, verPatch}; changed = true; }
  // git sha
  if (!j.contains("unrootGit") || !j["unrootGit"].is_string() || j["unrootGit"].get<std::string>() != gitSha) {
    j["unrootGit"] = gitSha; changed = true; }
  if (changed) {
    (void)atomicSaveMetaJson(rootfs, j);
  }
  return changed;
}

bool updateQemuWrapper(const fs::path& rootfs,
                       const std::string& emuBinary,
                       const std::vector<std::string>& emuArgs,
                       const std::string& gitSha) {
  try {
    auto j = loadMetaJson(rootfs);
    bool changed = false;
    // Ensure hostCaps snapshot present (idempotent)
    if (!j.contains("hostCaps")) {
      try { j["hostCaps"] = getGlobalHostCaps().toJson(true); changed = true; } catch (...) {}
    }
    nlohmann::json qw = nlohmann::json::object();
    if (j.contains("qemuWrapper") && j["qemuWrapper"].is_object()) {
      qw = j["qemuWrapper"];
    }
    auto setIf = [&](const std::string& k, const nlohmann::json& v){ if (!qw.contains(k) || qw[k] != v) { qw[k] = v; changed = true; } };
    setIf("emuBinary", emuBinary);
    setIf("emuArgs", emuArgs);
    util::Rootfs root(rootfs.string());
    struct stat st{};
    if (root && root.stat("/.unroot/bin/wrapper", st)) {
      setIf("wrapperDev", static_cast<long long>(st.st_dev));
      setIf("wrapperInode", static_cast<long long>(st.st_ino));
      setIf("wrapperSize", static_cast<long long>(st.st_size));
    }
    setIf("wrapperGit", gitSha);
    if (!j.contains("qemuWrapper") || j["qemuWrapper"] != qw) { j["qemuWrapper"] = qw; changed = true; }
    if (changed) return atomicSaveMetaJson(rootfs, j);
    return true;
  } catch (...) { return false; }
}

} // namespace meta
