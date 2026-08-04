// util/path.hpp — small path/process helpers for PATH lookups and executables
#pragma once

#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
// Keep heavy includes out of headers; implementation lives in src/util/path.cpp

namespace util {

inline bool pathExists(const std::string& p) {
  struct stat st{}; return ::stat(p.c_str(), &st) == 0;
}

inline bool isExecutableFile(const std::string& p) {
  struct stat st{}; if (::stat(p.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode)) return false;
  return ::access(p.c_str(), X_OK) == 0;
}

// Find an executable on PATH. If name contains '/', treat it as a path and return if executable.
inline std::string findOnPath(const std::string& name) {
  if (name.find('/') != std::string::npos) {
    return isExecutableFile(name) ? name : std::string();
  }
  const char* path = ::getenv("PATH"); if (!path || !*path) return {};
  std::string p(path); size_t start = 0;
  while (start <= p.size()) {
    size_t c = p.find(':', start);
    std::string d = p.substr(start, (c==std::string::npos ? p.size() : c) - start);
    if (!d.empty() && d != ".") {
      std::string cand = d + "/" + name;
      if (isExecutableFile(cand)) return cand;
    }
    if (c == std::string::npos) break;
    start = c + 1;
  }
  return {};
}

// Resolve sudo binary, honoring UNROOT_SUDO override.
inline std::string resolveSudoPath() {
  const char* env = ::getenv("UNROOT_SUDO");
  if (env && *env && isExecutableFile(env)) return std::string(env);
  if (isExecutableFile("/usr/bin/sudo")) return std::string("/usr/bin/sudo");
  return findOnPath("sudo");
}

// Resolve a command name within a rootfs context.
// - If name contains '/', it is treated as relative to cwdInRoot (if provided) or '/'.
// - Otherwise, pathEnv (a PATH string) is searched; absolute entries are interpreted inside rootfs,
//   and '.'/empty entries are resolved relative to cwdInRoot when provided.
// On success, returns an absolute path inside the rootfs (e.g., "/bridge-tools/bin/make").
// On failure, returns empty string and writes a human-readable error into outErr (if non-null).
std::string resolveExecInRoot(const std::string& root,
                              const std::string& cwdInRoot,
                              const std::string& name,
                              const std::string& pathEnv,
                              std::string* outErr = nullptr);

} // namespace util
