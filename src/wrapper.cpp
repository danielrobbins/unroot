#include <string>
#include <vector>
// Built-in wrapper implementation.
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/auxv.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include "wrapper.hpp"
#include "util/log_host.hpp"
#include "util/rootfs.hpp"
#include "meta.hpp"
#include "nlohmann/json.hpp"

using util::hostLogStep;

[[noreturn]] void runWrapper(int argc, char** argv) {
  // binfmt_misc P supplies: wrapper, executable, original argv[0], arguments.
  if (argc < 3) {
    std::fprintf(stderr, "[wrapper] missing executable or original program name\n");
    _exit(127);
  }
  // QEMU loads by pathname; this wrapper's exec loses AT_EXECFD. Close the
  // unused kernel-opened descriptor instead of leaking it into the guest.
  errno = 0;
  int execFd = static_cast<int>(::getauxval(AT_EXECFD));
  if (errno == 0) ::close(execFd);
  std::string emuBinStr;
  std::vector<std::string> emuArgs;
  // Read /.unroot/meta.json for qemuWrapper settings (env-based config deprecated)
  try {
    auto mj = meta::loadMetaJson("/");
    if (mj.contains("qemuWrapper") && mj["qemuWrapper"].is_object()) {
      auto &qw = mj["qemuWrapper"];
      if (qw.contains("emuBinary") && qw["emuBinary"].is_string()) emuBinStr = qw["emuBinary"].get<std::string>();
      if (qw.contains("emuArgs") && qw["emuArgs"].is_array()) {
        for (auto &v : qw["emuArgs"]) if (v.is_string()) emuArgs.push_back(v.get<std::string>());
      }
    }
  } catch (...) {
    // ignore
  }
  if (emuBinStr.empty()) {
    std::fprintf(stderr, "[wrapper] missing emulator configuration (meta.json)\n");
    _exit(127);
  }
  const char* emuBin = emuBinStr.c_str();
  bool debug = (::getenv("UNROOT_WRAPPER_DEBUG") != nullptr);
  std::vector<const char*> newv; newv.reserve((size_t)argc + emuArgs.size() + 8);
  newv.push_back(emuBin); // argv[0] = emulator
  for (const auto& argument : emuArgs) newv.push_back(argument.c_str());
  newv.push_back("-0");
  newv.push_back(argv[2]);
  // Use /proc/self/fd/255 only when that descriptor is the program itself. A
  // caller can leave an unrelated descriptor open at 255 (bash keeps its
  // script there and does not always set close-on-exec), and handing that to
  // QEMU makes it try to load the script as the target program.
  bool haveFd255 = false;
  if (!::getenv("UNROOT_QEMU_NOFD255")) {
    struct stat fdStat{}, pathStat{};
    haveFd255 = ::fstat(255, &fdStat) == 0 && ::stat(argv[1], &pathStat) == 0 &&
                fdStat.st_dev == pathStat.st_dev &&
                fdStat.st_ino == pathStat.st_ino;
  }
  newv.push_back(haveFd255 ? "/proc/self/fd/255" : argv[1]);
  for (int i=3;i<argc;i++) newv.push_back(argv[i]);
  newv.push_back(nullptr);
  if (debug) {
    std::fprintf(stderr, "[wrapper] emu=%s fd255=%d\n", emuBin, haveFd255?1:0);
    for (size_t i=0;i+1<newv.size(); ++i) std::fprintf(stderr, "[argv %zu]=%s\n", i, newv[i]);
  }
  for (const char* name : {"UNROOT_WRAPPER_DEBUG", "UNROOT_QEMU_NOFD255"}) {
    ::unsetenv(name);
  }
  ::execv(emuBin, const_cast<char* const*>(newv.data()));
  int err = errno;
  if (debug) std::fprintf(stderr, "[wrapper] execv failed errno=%d (%s)\n", err, std::strerror(err));
  _exit(127);
}

bool installSelfAsWrapper(const std::string& rootPath) {
  constexpr const char* destination = "/.unroot/bin/wrapper";
  util::Rootfs root(rootPath);
  if (!root || !root.directory("/.unroot/bin", true)) {
    hostLogStep("wrapper:dir", false, rootPath + "/.unroot/bin");
    return false;
  }
  char selfPath[PATH_MAX]; ssize_t n = ::readlink("/proc/self/exe", selfPath, sizeof(selfPath)-1); if (n <= 0) return false; selfPath[n]='\0';
  struct stat selfSt{}; if (::stat(selfPath, &selfSt) != 0) return false;
  // Fast path: if existing wrapper hard-linked to self (same dev+ino) skip
  struct stat dstSt{}; if (root.stat(destination, dstSt)) {
    if (dstSt.st_dev == selfSt.st_dev && dstSt.st_ino == selfSt.st_ino) {
      hostLogStep("wrapper:install", true, "hardlink-skip " + rootPath + destination);
      return true;
    }
  }
  // Attempt hard link first (same filesystem)
  if (root.linkHostFile(selfPath, destination)) {
    hostLogStep("wrapper:install", true, "hardlink " + rootPath + destination);
    return true;
  }
  // Fallback to copy
  bool ok = root.copyHostFileAtomic(selfPath, destination);
  hostLogStep("wrapper:install", ok,
              (ok ? "copy " : "copy failed ") + rootPath + destination);
  return ok;
}
