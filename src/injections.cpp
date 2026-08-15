#include "injections.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include "linuxns.hpp"
#include "nlohmann/json.hpp"
#include "util/host_helper.hpp"
#include "util/rootfs.hpp"

namespace injections {
namespace {

constexpr const char* RegistryPath = "/.unroot/injections/registry.json";
constexpr const char* RegistryVersion = "unroot.injections/v1";
constexpr const char* OriginalRoot = "/.unroot/injections/original";
constexpr const char* AbsentRoot = "/.unroot/injections/absent";

bool absolutePath(const std::string& path) {
  if (path.empty() || path.front() != '/' || path.find('\n') != std::string::npos)
    return false;
  size_t start = 1;
  while (start <= path.size()) {
    const size_t end = path.find('/', start);
    const std::string part = path.substr(
        start, (end == std::string::npos ? path.size() : end) - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
  return false;
}

bool reservedDestination(const std::string& path) {
  return path == "/.unroot" || path.rfind("/.unroot/", 0) == 0;
}

bool validName(const std::string& name) {
  if (name.empty()) return true;
  return std::all_of(name.begin(), name.end(), [](unsigned char value) {
    return std::isalnum(value) || value == '.' || value == '_' || value == '-';
  });
}

std::vector<std::string> fields(const std::string& value, char separator) {
  std::vector<std::string> result;
  size_t start = 0;
  while (true) {
    const size_t end = value.find(separator, start);
    result.push_back(value.substr(start, end == std::string::npos
                                            ? std::string::npos
                                            : end - start));
    if (end == std::string::npos) return result;
    start = end + 1;
  }
}

bool number(const std::string& text, unsigned int& value, int base = 10) {
  if (text.empty()) return false;
  const char* end = text.data() + text.size();
  auto parsed = std::from_chars(text.data(), end, value, base);
  return parsed.ec == std::errc() && parsed.ptr == end;
}

std::vector<Spec> builtins() {
  std::vector<Spec> result;
  Spec resolver;
  resolver.name = "resolv.conf";
  resolver.source = "/etc/resolv.conf";
  resolver.destination = "/etc/resolv.conf";
  result.push_back(std::move(resolver));
  Spec hosts;
  hosts.name = "hosts";
  hosts.source = "/etc/hosts";
  hosts.destination = "/etc/hosts";
  result.push_back(std::move(hosts));
  Spec mtab;
  mtab.name = "mtab";
  mtab.kind = Kind::Symlink;
  mtab.target = "/proc/self/mounts";
  mtab.destination = "/etc/mtab";
  mtab.mode = 0777;
  result.push_back(std::move(mtab));
  return result;
}

auto named(std::vector<Spec>& specs, const std::string& name) {
  return std::find_if(specs.begin(), specs.end(), [&](const Spec& spec) {
    return spec.name == name;
  });
}

std::string backupPath(const std::string& destination) {
  return std::string(OriginalRoot) + destination;
}

std::string absentPath(const std::string& destination) {
  return std::string(AbsentRoot) + destination;
}

bool mapped(const std::vector<util::IdMapExtent>& extents, unsigned int id) {
  return std::any_of(extents.begin(), extents.end(), [&](const auto& extent) {
    return id >= extent.inside && id - extent.inside < extent.count;
  });
}

bool prepareSources(std::vector<Spec>& specs, const util::IdMapPlan& idmap,
                    std::string& error) {
  std::set<std::string> names;
  std::set<std::string> destinations;
  for (auto& spec : specs) {
    if ((!spec.name.empty() && !names.insert(spec.name).second) ||
        !destinations.insert(spec.destination).second) {
      error = "duplicate injection destination or name: " + spec.destination;
      return false;
    }
    if (idmap.mode != util::IdMapMode::Native &&
        (!mapped(idmap.uids, spec.uid) || !mapped(idmap.gids, spec.gid))) {
      error = "injection ownership is outside the rootfs ID map: " +
              spec.destination;
      return false;
    }
    if (spec.kind == Kind::Symlink) {
      if (!absolutePath(spec.target)) {
        error = "invalid injection symlink target: " + spec.target;
        return false;
      }
      continue;
    }
    UniqueFd pinned(::open(spec.source.c_str(), O_PATH | O_CLOEXEC));
    struct stat info {};
    if (!pinned || ::fstat(pinned.get(), &info) != 0 || !S_ISREG(info.st_mode)) {
      error = "injection source does not resolve to a regular file: " +
              spec.source;
      return false;
    }
    const std::string descriptor = "/proc/self/fd/" +
                                   std::to_string(pinned.get());
    spec.sourceFd.reset(::open(descriptor.c_str(), O_RDONLY | O_CLOEXEC));
    if (!spec.sourceFd) {
      error = "unable to open pinned injection source: " + spec.source;
      return false;
    }
  }
  return true;
}

bool readRegistry(const util::Rootfs& root, std::vector<Entry>& entries,
                  std::string& error) {
  std::string content;
  if (!root.readText(RegistryPath, content)) {
    if (errno == ENOENT) return true;
    error = "unable to read the injection registry";
    return false;
  }
  try {
    const auto registry = nlohmann::json::parse(content);
    if (!registry.is_object() ||
        registry.value("version", "") != RegistryVersion ||
        !registry.contains("entries") || !registry["entries"].is_array())
      throw std::runtime_error("schema");
    std::set<std::string> names;
    std::set<std::string> destinations;
    for (const auto& value : registry["entries"]) {
      Entry entry;
      if (!value.is_object() || !value.contains("name") ||
          !value["name"].is_string() || !value.contains("destination") ||
          !value["destination"].is_string() || !value.contains("original") ||
          !value["original"].is_string() || !value.contains("uid") ||
          !value["uid"].is_number_unsigned() || !value.contains("gid") ||
          !value["gid"].is_number_unsigned() || !value.contains("mode") ||
          !value["mode"].is_number_unsigned())
        throw std::runtime_error("schema");
      entry.name = value["name"].get<std::string>();
      entry.destination = value["destination"].get<std::string>();
      entry.original = value["original"].get<std::string>();
      entry.uid = value["uid"].get<unsigned int>();
      entry.gid = value["gid"].get<unsigned int>();
      const unsigned int mode = value["mode"].get<unsigned int>();
      auto available = builtins();
      auto builtin = named(available, entry.name);
      const bool knownName = entry.name.empty() ||
                             (builtin != available.end() &&
                              builtin->destination == entry.destination);
      if (!validName(entry.name) || !absolutePath(entry.destination) ||
          reservedDestination(entry.destination) || !knownName ||
          (entry.original != "regular" && entry.original != "symlink" &&
           entry.original != "absent") ||
          mode > 0777 ||
          (!entry.name.empty() && !names.insert(entry.name).second) ||
          !destinations.insert(entry.destination).second)
        throw std::runtime_error("values");
      entry.mode = static_cast<mode_t>(mode);
      entries.push_back(std::move(entry));
    }
    return true;
  } catch (...) {
    error = "malformed injection registry";
    return false;
  }
}

bool writeRegistry(const util::Rootfs& root, const std::vector<Entry>& entries,
                   std::string& error) {
  if (entries.empty()) {
    if (root.remove(RegistryPath) || errno == ENOENT) return true;
    error = "unable to clear the injection registry";
    return false;
  }
  nlohmann::json values = nlohmann::json::array();
  for (const auto& entry : entries)
    values.push_back({{"name", entry.name},
                      {"destination", entry.destination},
                      {"original", entry.original},
                      {"uid", entry.uid},
                      {"gid", entry.gid},
                      {"mode", static_cast<unsigned int>(entry.mode)}});
  const nlohmann::json registry{{"version", RegistryVersion},
                                {"entries", std::move(values)}};
  if (root.writeTextAtomic(RegistryPath, registry.dump(2) + "\n", 0600))
    return true;
  error = "unable to write the injection registry";
  return false;
}

int runMapped(const std::string& rootfs, const util::IdMapPlan& idmap,
              std::vector<std::string> arguments, std::string& error) {
  const std::string helper = util::siblingHostHelper();
  if (helper.empty() || ::access(helper.c_str(), X_OK) != 0) {
    error = "unroot-util is not installed next to unroot";
    return -1;
  }
  arguments.insert(arguments.begin(), helper);
  const NsEnvVars environment{{"LC_ALL", "C"}};
  const NsResult result = enterNamespace({}, arguments, idmap, nullptr, nullptr,
                                         &environment, &rootfs);
  if (result.code == 0) return 0;
  error = result.msg.empty() ? "mapped injection helper failed" : result.msg;
  return result.code;
}

bool preserved(const util::Rootfs& root, const Entry& entry) {
  struct stat info {};
  if (entry.original == "absent")
    return root.lstat(absentPath(entry.destination), info) &&
           S_ISREG(info.st_mode);
  if (!root.lstat(backupPath(entry.destination), info)) return false;
  return entry.original == "regular" ? S_ISREG(info.st_mode)
                                     : S_ISLNK(info.st_mode);
}

bool restore(const std::string& rootfs, const util::IdMapPlan& idmap,
             const Entry& entry, std::string& error) {
  return runMapped(
             rootfs, idmap,
             {"injection", "restore", "--destination", entry.destination,
              "--backup", backupPath(entry.destination), "--absent",
              absentPath(entry.destination), "--original", entry.original},
             error) == 0;
}

bool install(const std::string& rootfs, const util::IdMapPlan& idmap,
             const util::Rootfs& root, Spec& spec, const Entry* existing,
             Entry& result, std::string& error) {
  UniqueFd source;
  std::vector<std::string> arguments{"injection", "install"};
  if (spec.kind == Kind::Regular) {
    source.reset(::fcntl(spec.sourceFd.get(), F_DUPFD, STDERR_FILENO + 1));
    if (!source) {
      error = "unable to pass injection source to unroot-util";
      return false;
    }
    arguments.insert(arguments.end(), {"--fd", std::to_string(source.get())});
  } else {
    arguments.insert(arguments.end(), {"--target", spec.target});
  }
  const std::string original = existing ? existing->original : "capture";
  arguments.insert(arguments.end(),
                   {"--destination", spec.destination, "--backup",
                    backupPath(spec.destination), "--absent",
                    absentPath(spec.destination), "--original", original,
                    "--uid", std::to_string(spec.uid), "--gid",
                    std::to_string(spec.gid), "--mode",
                    std::to_string(spec.mode)});
  if (runMapped(rootfs, idmap, std::move(arguments), error) != 0)
    return false;

  result.name = spec.name.empty() && existing ? existing->name : spec.name;
  result.destination = spec.destination;
  result.uid = spec.uid;
  result.gid = spec.gid;
  result.mode = spec.mode;
  if (existing) {
    result.original = existing->original;
    return true;
  }
  struct stat info {};
  if (root.lstat(backupPath(spec.destination), info)) {
    if (S_ISREG(info.st_mode)) result.original = "regular";
    else if (S_ISLNK(info.st_mode)) result.original = "symlink";
  } else if (root.lstat(absentPath(spec.destination), info) &&
             S_ISREG(info.st_mode)) {
    result.original = "absent";
  }
  if (!result.original.empty()) return true;
  error = "injection helper did not preserve the original destination: " +
          spec.destination;
  return false;
}

auto findDestination(std::vector<Entry>& entries, const std::string& path) {
  return std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
    return entry.destination == path;
  });
}

auto findTarget(std::vector<Entry>& entries, const std::string& target) {
  return std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
    return entry.name == target || entry.destination == target;
  });
}

}  // namespace

std::vector<Spec> defaults(const std::vector<std::string>& disabled) {
  auto result = builtins();
  if (std::find(disabled.begin(), disabled.end(), "*") != disabled.end())
    return {};
  result.erase(std::remove_if(result.begin(), result.end(), [&](const Spec& spec) {
                 return std::find(disabled.begin(), disabled.end(), spec.name) !=
                        disabled.end();
               }),
               result.end());
  return result;
}

bool parseSpec(const std::string& value, Spec& spec, std::string& error) {
  auto available = builtins();
  auto item = named(available, value);
  if (item != available.end()) {
    spec = std::move(*item);
    return true;
  }
  const auto parts = fields(value, ':');
  if (parts.size() != 1 && parts.size() != 2 && parts.size() != 5) {
    error = "custom injection expects SOURCE[:DESTINATION[:UID:GID:MODE]]";
    return false;
  }
  spec.source = parts[0];
  spec.destination = parts.size() == 1 ? parts[0] : parts[1];
  unsigned int mode = 0644;
  if (!absolutePath(spec.source) || !absolutePath(spec.destination) ||
      reservedDestination(spec.destination) ||
      (parts.size() == 5 &&
       (!number(parts[2], spec.uid) || spec.uid > util::MaxMappedId ||
        !number(parts[3], spec.gid) || spec.gid > util::MaxMappedId ||
         !number(parts[4], mode, 8) || mode > 0777))) {
    error = "custom injection requires absolute non-.unroot paths, numeric "
            "IDs, and an octal mode between 0000 and 0777";
    return false;
  }
  spec.mode = static_cast<mode_t>(mode);
  return true;
}

bool parseUnpackExclusions(const std::string& value,
                           std::vector<std::string>& disabled,
                           std::string& error) {
  for (const auto& token : fields(value, ',')) {
    if (token == "-*") {
      if (std::find(disabled.begin(), disabled.end(), "*") == disabled.end())
        disabled.push_back("*");
      continue;
    }
    if (token.size() < 2 || token.front() != '-') {
      error = "unpack --inject accepts only exclusions such as -hosts or -*; "
              "use 'unroot inject add' for custom injections";
      return false;
    }
    const std::string name = token.substr(1);
    auto available = builtins();
    if (named(available, name) == available.end()) {
      error = "unknown default injection '" + name + "'";
      return false;
    }
    if (std::find(disabled.begin(), disabled.end(), name) == disabled.end())
      disabled.push_back(name);
  }
  return true;
}

bool list(const std::string& rootfs, std::vector<Entry>& entries,
          std::string& error) {
  util::Rootfs root(rootfs);
  if (!root) {
    error = "unable to open the managed rootfs";
    return false;
  }
  return readRegistry(root, entries, error);
}

bool add(const std::string& rootfs, const util::IdMapPlan& idmap,
         std::vector<Spec> specs, std::string& error) {
  if (!prepareSources(specs, idmap, error)) return false;
  util::Rootfs root(rootfs);
  if (!root || root.isHostRoot()) {
    error = "unable to open a distinct managed rootfs";
    return false;
  }
  std::vector<Entry> entries;
  if (!readRegistry(root, entries, error)) return false;
  const std::string pinned = util::Rootfs::fdPath(root.fd());
  for (auto& spec : specs) {
    auto existing = findDestination(entries, spec.destination);
    if (existing != entries.end() && !preserved(root, *existing)) {
      error = "preserved original is missing for " + spec.destination;
      return false;
    }
    Entry installed;
    if (!install(pinned, idmap, root, spec,
                 existing == entries.end() ? nullptr : &*existing,
                 installed, error))
      return false;
    if (existing == entries.end()) entries.push_back(std::move(installed));
    else *existing = std::move(installed);
    if (!writeRegistry(root, entries, error)) return false;
  }
  return true;
}

bool remove(const std::string& rootfs, const util::IdMapPlan& idmap,
            const std::vector<std::string>& targets, std::string& error) {
  util::Rootfs root(rootfs);
  if (!root || root.isHostRoot()) {
    error = "unable to open a distinct managed rootfs";
    return false;
  }
  std::vector<Entry> entries;
  if (!readRegistry(root, entries, error)) return false;
  std::set<std::string> destinations;
  for (const auto& target : targets) {
    auto entry = findTarget(entries, target);
    if (entry == entries.end()) {
      error = "injection is not registered: " + target;
      return false;
    }
    if (!destinations.insert(entry->destination).second) {
      error = "injection was selected more than once: " + target;
      return false;
    }
    if (!preserved(root, *entry)) {
      error = "preserved original is missing for " + entry->destination;
      return false;
    }
  }
  const std::string pinned = util::Rootfs::fdPath(root.fd());
  for (const auto& target : targets) {
    auto entry = findTarget(entries, target);
    if (!restore(pinned, idmap, *entry, error)) return false;
    entries.erase(entry);
    if (!writeRegistry(root, entries, error)) return false;
  }
  return true;
}

bool clear(const std::string& rootfs, const util::IdMapPlan& idmap,
           std::string& error) {
  util::Rootfs root(rootfs);
  if (!root || root.isHostRoot()) {
    error = "unable to open a distinct managed rootfs";
    return false;
  }
  std::vector<Entry> entries;
  if (!readRegistry(root, entries, error)) return false;
  for (const auto& entry : entries) {
    if (!preserved(root, entry)) {
      error = "preserved original is missing for " + entry.destination;
      return false;
    }
  }
  const std::string pinned = util::Rootfs::fdPath(root.fd());
  while (!entries.empty()) {
    if (!restore(pinned, idmap, entries.back(), error)) return false;
    entries.pop_back();
    if (!writeRegistry(root, entries, error)) return false;
  }
  return true;
}

ArchivePlan archivePlan(const std::string& rootfs) {
  ArchivePlan plan;
  util::Rootfs root(rootfs);
  if (!root) {
    plan.error = "unable to open managed rootfs injections";
    return plan;
  }
  std::vector<Entry> entries;
  if (!readRegistry(root, entries, plan.error)) return plan;
  for (const auto& entry : entries) {
    if (!preserved(root, entry)) {
      plan.error = "preserved original is missing for " + entry.destination;
      return plan;
    }
    plan.excludes.push_back(entry.destination.substr(1));
    if (entry.original != "absent")
      plan.substitutions.push_back(
          {backupPath(entry.destination).substr(1),
           entry.destination.substr(1)});
  }
  return plan;
}

}  // namespace injections
