#pragma once

#include <fnmatch.h>
#include <string>
#include <vector>

namespace archiveio {

// Patterns are anchored at the rootfs. A matching directory excludes its
// descendants too, including preserved injection originals added separately.
inline bool excludedByPattern(const std::string& path,
                              const std::vector<std::string>& patterns) {
  for (const auto& pattern : patterns) {
    std::string candidate = path;
    while (!candidate.empty()) {
      if (::fnmatch(pattern.c_str(), candidate.c_str(), FNM_PATHNAME) == 0)
        return true;
      const auto slash = candidate.rfind('/');
      if (slash == std::string::npos) break;
      candidate.resize(slash);
    }
  }
  return false;
}

}  // namespace archiveio
