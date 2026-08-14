#include "host_helper.hpp"

#include <cerrno>
#include <climits>
#include <filesystem>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/fd.hpp"

namespace util {

std::string siblingHostHelper() {
  char path[PATH_MAX];
  const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length < 0) return {};
  path[length] = '\0';
  return (std::filesystem::path(path).parent_path() / "unroot-util").string();
}

HostHelperResult runHostHelper(const std::vector<std::string>& arguments,
                               size_t outputLimit,
                               const std::string& helperPath) {
  const std::string helper =
      helperPath.empty() ? siblingHostHelper() : helperPath;
  if (helper.empty() || ::access(helper.c_str(), X_OK) != 0)
    return {-1, {}, "unroot-util is not installed next to unroot"};

  int output[2];
  if (::pipe2(output, O_CLOEXEC) != 0)
    return {-1, {}, "unable to communicate with unroot-util"};
  UniqueFd readEnd(output[0]);
  UniqueFd writeEnd(output[1]);

  const pid_t child = ::fork();
  if (child == 0) {
    readEnd.reset();
    const int descriptor = writeEnd.release();
    if ((descriptor != STDOUT_FILENO &&
         ::dup2(descriptor, STDOUT_FILENO) < 0) ||
        (descriptor != STDERR_FILENO &&
         ::dup2(descriptor, STDERR_FILENO) < 0))
      _exit(126);
    if (descriptor > STDERR_FILENO) ::close(descriptor);

    std::vector<std::string> command{"unroot-util"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (auto& argument : command) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execv(helper.c_str(), argv.data());
    _exit(127);
  }
  writeEnd.reset();
  if (child < 0) return {-1, {}, "unable to start unroot-util"};

  HostHelperResult result;
  char buffer[512];
  while (true) {
    const ssize_t length = ::read(readEnd.get(), buffer, sizeof(buffer));
    if (length < 0 && errno == EINTR) continue;
    if (length <= 0) break;
    const size_t available = result.output.size() < outputLimit
                                 ? outputLimit - result.output.size()
                                 : 0;
    const size_t count = static_cast<size_t>(length);
    result.output.append(buffer, count < available ? count : available);
    if (count > available) result.truncated = true;
  }
  readEnd.reset();

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) {
    result.error = "unable to wait for unroot-util";
  } else if (WIFEXITED(status)) {
    result.code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.code = 128 + WTERMSIG(status);
  } else {
    result.error = "unroot-util returned an unknown process status";
  }
  return result;
}

}  // namespace util
