#pragma once

#include <filesystem>

#include "filesystem_caps.hpp"

namespace fsinfo {

class FilesystemInspector {
 public:
  Result inspect(const std::filesystem::path& root) const;
};

}  // namespace fsinfo
