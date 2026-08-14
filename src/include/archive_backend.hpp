#pragma once

#include <filesystem>
#include <string>

#include "archive_input.hpp"
#include "util/idmap.hpp"

namespace archive {

class Backend {
 public:
  Backend();

  explicit operator bool() const { return error_.empty(); }
  const std::string& error() const { return error_; }

  int create(const std::filesystem::path& root,
             const util::IdMapPlan& idmap, int outputDescriptor,
             const std::filesystem::path& destination, bool force) const;
  int extract(const std::filesystem::path& root,
              const util::IdMapPlan& idmap,
              const archiveio::Input& input, bool force) const;

 private:
  std::string helper_;
  std::string error_;
};

}  // namespace archive
