#pragma once

#include <cstddef>
#include <string>

namespace fsinfo {

inline constexpr const char* Protocol = "unroot-filesystem-v1";
inline constexpr size_t ResponseLimit = 4096;

struct Capability {
  bool supported = false;
  std::string detail;
};

struct FilesystemCaps {
  Capability posixAcl;
  Capability xattr;
};

struct Result {
  FilesystemCaps caps;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

std::string makeRecord(const FilesystemCaps& caps);
Result parseRecord(const std::string& record);

}  // namespace fsinfo
