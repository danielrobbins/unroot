// util/error_map.hpp — map util::Error to normalized app::Exit
#pragma once

#include <errno.h>
#include "util/errors.hpp"
#include "util/app_exit.hpp"

namespace app {

inline Exit mapErrorToExit(const util::Error& e) {
  using util::LibErr;
  switch (e.code) {
    case LibErr::NotFound: return Exit::NotFound;
    case LibErr::Io:
      // Special-case common not-found errno when Io is used as a transport
      if (e.sys_errno == ENOENT) return Exit::NotFound;
      return Exit::Io;
    case LibErr::Permission: return Exit::Io; // permission denied treated as I/O-ish failure
    case LibErr::Namespace: return Exit::Ns;
    case LibErr::Binfmt: return Exit::Unknown;
    case LibErr::Invalid: return Exit::Usage; // invalid CLI input or file format
    case LibErr::Syscall: return Exit::Io;
    case LibErr::Unknown: default: return Exit::Unknown;
  }
}

} // namespace app
