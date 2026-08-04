#include <filesystem>
#include <system_error>
#include <string>
#include "util/path.hpp"

namespace util {

std::string resolveExecInRoot(const std::string& root,
                              const std::string& cwdInRoot,
                              const std::string& name,
                              const std::string& pathEnv,
                              std::string* outErr) {
  namespace fs = std::filesystem;
  auto emitErr = [&](const std::string& msg){ if (outErr) *outErr = msg; };
  auto cwdDisp = [&](){ return cwdInRoot.empty()? std::string("/") : cwdInRoot; };

  if (!name.empty() && name.find('/') != std::string::npos) {
    fs::path base = fs::path(root);
    if (!cwdInRoot.empty()) base /= (cwdInRoot[0] == '/' ? fs::path(cwdInRoot.substr(1)) : fs::path(cwdInRoot));
    fs::path cand = base / name;
    std::error_code ec; auto st = fs::status(cand, ec);
    if (ec || !fs::exists(st) || !fs::is_regular_file(st)) {
      emitErr(std::string("ERROR: Command '") + name + "' not found relative to cwd '" + cwdDisp() + "'.");
      return {};
    }
    auto perms = st.permissions();
    if ((perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) == fs::perms::none) {
      emitErr(std::string("ERROR: Command '") + name + "' is not executable relative to cwd '" + cwdDisp() + "'.");
      return {};
    }
    fs::path absInRoot = (cwdInRoot.empty()?fs::path("/"):fs::path(cwdInRoot)) / name;
    return absInRoot.lexically_normal().string();
  }

  // Bare name: need PATH
  if (pathEnv.empty()) {
    emitErr(std::string("ERROR: Command '") + name + "' is not an absolute path and no PATH was provided via --env.");
    return {};
  }

  std::string paths = pathEnv; size_t start=0;
  while (start <= paths.size()) {
    size_t c = paths.find(':', start);
    std::string d = paths.substr(start, (c==std::string::npos?paths.size():c) - start);
    fs::path baseInRoot = fs::path(root);
    bool useDot = (d.empty() || d == ".");
    if (useDot) {
      if (!cwdInRoot.empty()) {
        baseInRoot /= (cwdInRoot[0]=='/'?fs::path(cwdInRoot.substr(1)):fs::path(cwdInRoot));
      } else {
        goto next_path_entry;
      }
    } else {
      if (!d.empty() && d[0]=='/') baseInRoot /= fs::path(d.substr(1));
      else baseInRoot /= fs::path(d);
    }
    {
      fs::path p = baseInRoot / name;
      std::error_code ec; auto st = fs::status(p, ec);
      if (!ec && fs::exists(st) && fs::is_regular_file(st)) {
        auto perms = st.permissions();
        if ((perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none) {
          fs::path absInRoot = useDot ? (fs::path(cwdDisp()) / name)
                                      : ((d.empty()?fs::path(""):fs::path(d)) / name);
          return absInRoot.is_absolute()? absInRoot.lexically_normal().string()
                                        : std::string("/") + absInRoot.lexically_normal().string();
        }
      }
    }
  next_path_entry:
    if (c == std::string::npos) break;
    start = c + 1;
  }
  emitErr(std::string("ERROR: Command '") + name + "' not found in PATH '" + pathEnv + "'.");
  return {};
}

} // namespace util
