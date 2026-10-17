#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace util {

struct Sha256Result {
  std::string hex;
  uint64_t size = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

Sha256Result sha256File(int descriptor);
Sha256Result sha256File(const std::filesystem::path& path);

}  // namespace util
