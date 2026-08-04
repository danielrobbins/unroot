#include <vector>
#include <string>
#include <utility>
#include <unistd.h>
#include <sys/stat.h>
#include "util/rootfs.hpp"
#include "shebang.hpp"

using namespace std;
namespace fs = std::filesystem;

static bool isExecutable(const util::Rootfs& root, const std::string& path) {
  UniqueFd file = root.resolvedFile(path);
  struct stat info{};
  return file && ::fstat(file.get(), &info) == 0 && S_ISREG(info.st_mode) &&
         (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}

static std::string parseShebangInterpreter(const util::Rootfs& root,
                                           const std::string& pathInRoot) {
  if (pathInRoot.empty() || pathInRoot[0] != '/') return {};
  UniqueFd file = root.resolvedFile(pathInRoot);
  if (!file) return {};
  char buf[256]; ssize_t n = ::read(file.get(), buf, sizeof(buf)-1);
  if (n <= 3) return {};
  buf[n] = '\0';
  if (!(buf[0]=='#' && buf[1]=='!')) return {};
  const char* p = buf + 2; while (*p==' '||*p=='\t') ++p;
  std::string line(p);
  size_t nl = line.find('\n'); if (nl != std::string::npos) line.resize(nl);
  vector<string> toks; string cur; for (char c : line) { if (c==' '||c=='\t') { if(!cur.empty()){toks.push_back(cur);cur.clear();} } else cur.push_back(c);} if(!cur.empty()) toks.push_back(cur);
  if (toks.empty()) return {};
  string interp = toks[0];
  if (interp == "/usr/bin/env" && toks.size() >= 2) {
    string name = toks[1];
    vector<string> dirs = {"/bridge-tools/bin","/usr/bin","/bin","/usr/local/bin"};
    for (auto& d : dirs) {
      std::string candidate = d + "/" + name;
      if (isExecutable(root, candidate)) return candidate;
    }
    return {};
  }
  if (!interp.empty() && interp[0] == '/') return interp;
  return {};
}

std::string resolveExecutableImage(const fs::path& root,
                                   const std::string& pathInRoot) {
  util::Rootfs rootfs(root.string());
  if (!rootfs) return {};
  std::string current = pathInRoot;
  for (int depth = 0; depth < 8; ++depth) {
    std::string interpreter = parseShebangInterpreter(rootfs, current);
    if (interpreter.empty()) return current;
    if (interpreter == current) return {};
    current = std::move(interpreter);
  }
  return {};
}
