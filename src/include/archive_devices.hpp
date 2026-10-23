#pragma once

#include <string>

namespace archiveio {

// An empty path leaves an existing root alone. A managed path with no
// descriptor discards its contents; a descriptor preserves/restores them.
struct DeviceTree {
  std::string path;
  int descriptor = -1;
};

inline constexpr const char* DeviceArchive = "/.unroot/dev.tar";
inline constexpr const char* DeviceArchiveTemporary = "/.unroot/dev.tar.tmp";

}  // namespace archiveio
