#include "archive_fd.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace util {

UniqueFd reopenArchiveDescriptor(int descriptor, int flags,
                                 std::string& error) {
  struct stat original {};
  if (::fstat(descriptor, &original) != 0 || !S_ISREG(original.st_mode)) {
    error = "archive descriptor is not a regular file";
    return {};
  }
  UniqueFd reopened(::open(("/proc/self/fd/" + std::to_string(descriptor)).c_str(),
                           flags | O_CLOEXEC));
  struct stat current {};
  if (!reopened || ::fstat(reopened.get(), &current) != 0 ||
      current.st_dev != original.st_dev || current.st_ino != original.st_ino) {
    error = "unable to pin archive descriptor";
    return {};
  }
  return reopened;
}

uint64_t archiveDescriptorSize(int descriptor) {
  struct stat info {};
  return ::fstat(descriptor, &info) == 0 && info.st_size > 0
             ? static_cast<uint64_t>(info.st_size)
             : 0;
}

}  // namespace util
