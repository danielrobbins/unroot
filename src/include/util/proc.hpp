// Lightweight process helpers for direct fork/exec/wait.
#pragma once

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>
#include <vector>

namespace util {

struct CapturedProcess {
  int code;
  std::string output;
};

inline CapturedProcess capture_execv(const std::string& path,
                                     const std::vector<std::string>& args) {
  int output[2];
  if (::pipe(output) != 0) return {-1, {}};
  pid_t p = ::fork();
  if (p == 0) {
    ::close(output[0]);
    if (::dup2(output[1], STDOUT_FILENO) < 0 ||
        ::dup2(output[1], STDERR_FILENO) < 0)
      _exit(126);
    if (output[1] > STDERR_FILENO) ::close(output[1]);
    std::vector<char*> cargs;
    cargs.reserve(args.size() + 1);
    for (const auto& s : args) cargs.push_back(const_cast<char*>(s.c_str()));
    cargs.push_back(nullptr);
    ::execv(path.c_str(), cargs.data());
    _exit(127);
  }
  ::close(output[1]);
  if (p < 0) {
    ::close(output[0]);
    return {-1, {}};
  }
  std::string captured;
  bool truncated = false;
  char buffer[512];
  for (;;) {
    ssize_t count = ::read(output[0], buffer, sizeof(buffer));
    if (count > 0) {
      constexpr size_t limit = 65536;
      const size_t available = captured.size() < limit ? limit - captured.size() : 0;
      const size_t append = std::min(static_cast<size_t>(count), available);
      captured.append(buffer, append);
      truncated = truncated || append < static_cast<size_t>(count);
    } else if (count == 0)
      break;
    else if (errno != EINTR)
      break;
  }
  ::close(output[0]);
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(p, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (truncated) captured += "\n[output truncated]\n";
  if (waited != p) return {-1, std::move(captured)};
  if (WIFEXITED(status)) return {WEXITSTATUS(status), std::move(captured)};
  if (WIFSIGNALED(status))
    return {128 + WTERMSIG(status), std::move(captured)};
  return {-1, std::move(captured)};
}

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
