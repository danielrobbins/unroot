#pragma once

#include <filesystem>
#include <string>

#include "util/fd.hpp"

namespace archiveio {

class Input {
 public:
  explicit Input(const std::filesystem::path& path);

  explicit operator bool() const { return error_.empty(); }
  const std::string& error() const { return error_; }
  UniqueFd duplicateForChild() const;

 private:
  UniqueFd descriptor_;
  std::string error_;
};

}  // namespace archiveio
