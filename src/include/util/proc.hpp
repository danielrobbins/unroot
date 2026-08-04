// Lightweight process helpers for direct fork/exec/wait.
#pragma once

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

namespace util {

// Spawn a process with execv and wait for completion. argv0 should be args[0].
inline int spawn_and_wait_execv(const std::string& path,
                                const std::vector<std::string>& args,
                                bool quiet = false) {
  pid_t p = ::fork();
  if (p == 0) {
    // child
    if (quiet) {
      int null = ::open("/dev/null", O_RDWR);
      if (null < 0 || ::dup2(null, STDOUT_FILENO) < 0 ||
          ::dup2(null, STDERR_FILENO) < 0)
        _exit(126);
      if (null > STDERR_FILENO) ::close(null);
    }
    std::vector<char*> cargs;
    cargs.reserve(args.size() + 1);
    for (const auto& s : args) cargs.push_back(const_cast<char*>(s.c_str()));
    cargs.push_back(nullptr);
    ::execv(path.c_str(), cargs.data());
    _exit(127);
  }
  if (p < 0) return -1;
  int st = 0;
  pid_t waited;
  do {
    waited = ::waitpid(p, &st, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != p) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

}  // namespace util
