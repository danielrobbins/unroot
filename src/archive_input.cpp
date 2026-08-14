#include "archive_input.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace archiveio {

Input::Input(const std::filesystem::path& path)
    : descriptor_(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)) {
  struct stat info {};
  if (!descriptor_)
    error_ = "unable to open archive: " + path.string();
  else if (::fstat(descriptor_.get(), &info) != 0 || !S_ISREG(info.st_mode))
    error_ = "archive is not a regular file: " + path.string();
}

UniqueFd Input::duplicateForChild() const {
  return UniqueFd(::fcntl(descriptor_.get(), F_DUPFD, STDERR_FILENO + 1));
}

}  // namespace archiveio
