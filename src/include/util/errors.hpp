// util/errors.hpp — library error taxonomy
#pragma once

#include <string>

namespace util {

enum class LibErr {
  Io,
  NotFound,
  Invalid,
  Permission,
  Syscall,
  Namespace,
  Binfmt,
  Unknown,
};

struct Error {
  LibErr code = LibErr::Unknown;
  int sys_errno = 0;       // optional errno from failing syscall
  std::string message;     // human context (no user printing here)
};

inline Error make_error(LibErr c, int e, std::string msg) {
  return Error{c, e, std::move(msg)};
}

} // namespace util
