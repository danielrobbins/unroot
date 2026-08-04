// util/app_exit.hpp — normalized app exit codes
#pragma once

namespace app {

enum class Exit : int {
  Ok = 0,
  Usage = 2,
  NotFound = 3,
  Io = 5,
  Prepare = 10,
  Exec = 12,
  FsFailure = 15,  // Filesystem operation failure
  Ns = 20,
  ChildSignaled = 25,
  Unknown = 30,
};

inline int toInt(Exit e) { return static_cast<int>(e); }

} // namespace app
