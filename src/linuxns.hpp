// linuxns.hpp — minimal namespace helper for unroot (C++17)
#pragma once
#include <string>
#include <vector>
#include <sys/stat.h> // For mkdir
#include <unistd.h>   // For ::mkdir
#include "util/idmap.hpp"

struct NsResult {
  int code;
  std::string msg;
};

// Optional QEMU emulation plan passed from the CLI layer.
struct EmuPlan {
  std::string source;
  std::string target;
  std::string registration;
};

// User-requested bind mounts.
struct BindMap {
  std::string src;
  std::string dst;
  bool readonly = false;
};

using NsEnvVars = std::vector<std::pair<std::string,std::string>>;

NsResult enterNamespace(const std::string& rootfs,
                        const std::vector<std::string>& shellArgv,
                        const util::IdMapPlan& idmap,
                        const EmuPlan* emu = nullptr,
                        const std::vector<BindMap>* maps = nullptr,
                        const NsEnvVars* envVars = nullptr,
                        const std::string* cwdInRoot = nullptr);

std::vector<std::string> resolveFeatureNamesFromEnv();

// Utility function to create a directory and all its parents.
inline bool ensureDirAll(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  std::string cur;
  cur.reserve(path.size());
  for (size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    cur.push_back(c);
    if (c == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
  }
  ::mkdir(path.c_str(), 0755);
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
