#include "exclusions.hpp"

#include <algorithm>
#include <cerrno>
#include <set>
#include <stdexcept>

#include "nlohmann/json.hpp"
#include "util/rootfs.hpp"

namespace exclusions {
namespace {
constexpr const char* Registry = "/.unroot/exclusions.json";
constexpr const char* Version = "unroot.exclusions/v1";

bool validPattern(const std::string& pattern) {
  if (pattern.size() < 2 || pattern.front() != '/' ||
      pattern.find_first_of("\n\r\t\\") != std::string::npos ||
      pattern.find('\0') != std::string::npos ||
      pattern.find("**") != std::string::npos ||
      pattern == "/.unroot" || pattern.rfind("/.unroot/", 0) == 0)
    return false;
  size_t start = 1;
  while (start <= pattern.size()) {
    const auto end = pattern.find('/', start);
    const auto part = pattern.substr(start, end == std::string::npos
                                               ? end : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string::npos) return true;
    start = end + 1;
  }
  return false;
}
}  // namespace

bool list(const std::string& rootfs, std::vector<std::string>& patterns,
          std::string& error) {
  util::Rootfs root(rootfs);
  std::string text;
  if (!root || !root.readText(Registry, text)) {
    if (root && errno == ENOENT) return true;
    error = "unable to read exclusion registry";
    return false;
  }
  try {
    const auto data = nlohmann::json::parse(text);
    if (data.at("version") != Version || !data.at("patterns").is_array())
      throw std::runtime_error("schema");
    const auto values = data.at("patterns").get<std::vector<std::string>>();
    std::set<std::string> unique;
    for (const auto& value : values)
      if (!validPattern(value) || !unique.insert(value).second)
        throw std::runtime_error("invalid pattern");
    patterns = values;
    return true;
  } catch (const std::exception&) {
    error = "malformed exclusion registry";
    return false;
  }
}

bool update(const std::string& rootfs, const std::string& operation,
            const std::vector<std::string>& patterns, std::string& error) {
  util::Rootfs root(rootfs);
  if (!root || root.isHostRoot()) {
    error = "exclusions require a rootfs distinct from the host root";
    return false;
  }
  std::vector<std::string> entries;
  if (!list(rootfs, entries, error)) return false;
  for (const auto& pattern : patterns) {
    if (!validPattern(pattern)) {
      error = "exclusion requires a rootfs-absolute path or glob, without "
              "dot components, .unroot, escapes, or **: " + pattern;
      return false;
    }
    auto found = std::find(entries.begin(), entries.end(), pattern);
    if (operation == "add") {
      if (found == entries.end()) entries.push_back(pattern);
    } else if (operation == "remove") {
      if (found == entries.end()) {
        error = "exclusion is not registered: " + pattern;
        return false;
      }
      entries.erase(found);
    } else {
      error = "unknown exclusion operation";
      return false;
    }
  }
  if (operation == "clear") entries.clear();
  if (entries.empty()) {
    if (root.remove(Registry) || errno == ENOENT) return true;
  } else {
    const nlohmann::json data{{"version", Version}, {"patterns", entries}};
    if (root.writeTextAtomic(Registry, data.dump(2) + "\n", 0600)) return true;
  }
  error = "unable to write exclusion registry";
  return false;
}

}  // namespace exclusions
