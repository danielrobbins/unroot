#include "injection_action.hpp"

#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>

#include "app_exception.hpp"
#include "injection_config.hpp"
#include "injections.hpp"
#include "meta.hpp"
#include "nlohmann/json.hpp"
#include "util/archive_lock.hpp"
#include "util/error_map.hpp"
#include "util/idmap.hpp"
#include "util/rootfs.hpp"

namespace actions {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) {
  throw AppException(util::make_error(util::LibErr::Invalid, 0, message),
                     "inject");
}

fs::path absolutePath(const fs::path& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  if (error) fail("unable to resolve root path: " + path.string());
  fs::path result = fs::weakly_canonical(absolute, error);
  if (error) fail("unable to resolve root path: " + path.string());
  return result;
}

util::IdMapPlan resolveMap(const fs::path& root,
                           const meta::IdMapMetaResult& stored) {
  auto resolved = meta::resolveIdMap(root, stored.plan.mode,
                                     util::subordinateIdCount(stored.plan),
                                     false);
  if (!resolved) fail("idmap: " + resolved.error);
  return std::move(resolved.plan);
}

std::string modeString(mode_t mode) {
  std::ostringstream output;
  output << '0' << std::oct << std::setfill('0') << std::setw(3)
         << static_cast<unsigned int>(mode);
  return output.str();
}

std::string currentType(const util::Rootfs& root,
                        const injections::Entry& entry) {
  struct stat info {};
  if (!root.lstat(entry.destination, info))
    return errno == ENOENT ? "missing" : "unreadable";
  if (S_ISREG(info.st_mode)) return "present";
  if (S_ISLNK(info.st_mode)) return "symlink";
  if (S_ISDIR(info.st_mode)) return "directory";
  return "special";
}

void displayList(const fs::path& rootPath,
                 const std::vector<injections::Entry>& entries, bool json) {
  util::Rootfs root(rootPath.string());
  if (json) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& entry : entries)
      values.push_back({{"name", entry.name.empty()
                                    ? nlohmann::json(nullptr)
                                    : nlohmann::json(entry.name)},
                        {"destination", entry.destination},
                        {"owner", {entry.uid, entry.gid}},
                        {"mode", modeString(entry.mode)},
                        {"original", entry.original},
                        {"current", currentType(root, entry)}});
    std::cout << nlohmann::json{{"version", "unroot.injections/v1"},
                               {"entries", std::move(values)}}
                         .dump(2)
              << '\n';
    return;
  }
  if (entries.empty()) {
    std::cout << "No injections registered.\n";
    return;
  }
  std::cout << std::left << std::setw(16) << "NAME" << std::setw(28)
            << "DESTINATION" << std::setw(12) << "OWNER" << std::setw(8)
            << "MODE" << std::setw(10) << "ORIGINAL" << "CURRENT\n";
  for (const auto& entry : entries) {
    const std::string owner = std::to_string(entry.uid) + ":" +
                              std::to_string(entry.gid);
    std::cout << std::left << std::setw(16)
              << (entry.name.empty() ? "(custom)" : entry.name)
              << std::setw(28) << entry.destination << std::setw(12) << owner
              << std::setw(8) << modeString(entry.mode) << std::setw(10)
              << entry.original << currentType(root, entry) << '\n';
  }
}

}  // namespace

int InjectionAction::perform(const InjectionConfig& config) {
  const fs::path root = absolutePath(config.root);
  auto rootLock = util::acquireArchiveLock(root, false);
  if (!rootLock)
    fail(rootLock.busy ? "another rootfs operation is active for ROOT"
                       : rootLock.error);
  if (!rootLock.matchesRoot()) fail("ROOT changed during injection operation");
  const fs::path pinnedRoot = rootLock.pinnedRoot();
  auto stored = meta::readIdMap(pinnedRoot);
  if (!stored.error.empty()) fail("idmap: " + stored.error);
  if (!stored.found)
    fail("inject requires a managed rootfs created by unroot unpack");

  std::string error;
  if (config.operation == "list") {
    std::vector<injections::Entry> entries;
    if (!injections::list(pinnedRoot.string(), entries, error)) fail(error);
    displayList(pinnedRoot, entries, config.json);
    return 0;
  }

  auto idmap = resolveMap(pinnedRoot, stored);
  if (config.operation == "add") {
    std::vector<injections::Spec> specs;
    for (const auto& value : config.items) {
      injections::Spec spec;
      if (!injections::parseSpec(value, spec, error)) fail(error);
      specs.push_back(std::move(spec));
    }
    if (!injections::add(pinnedRoot.string(), idmap, std::move(specs), error))
      fail(error);
  } else if (config.operation == "remove") {
    if (!injections::remove(pinnedRoot.string(), idmap, config.items, error))
      fail(error);
  } else if (!injections::clear(pinnedRoot.string(), idmap, error)) {
    fail(error);
  }
  return 0;
}

}  // namespace actions
