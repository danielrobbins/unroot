// util/file.hpp — small file helpers built on util/io and util/fd
#pragma once

#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "util/fd.hpp"
#include "util/io.hpp"

namespace util {

inline std::string readSmallFile(const std::string& path, size_t maxBytes = 2048) {
  UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) return {};
  std::string out; out.resize(maxBytes);
  ssize_t n = util::read_n(fd.get(), out.data(), out.size());
  if (n <= 0) return {};
  out.resize(static_cast<size_t>(n));
  return out;
}

inline bool writeFileText(const std::string& path, const std::string& content, mode_t mode = 0644) {
  UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode));
  if (!fd) return false;
  return util::write_all(fd.get(), content.data(), content.size());
}

} // namespace util
