#include "util/sha256.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include "picosha2.h"
#include "util/fd.hpp"

namespace util {

Sha256Result sha256File(int descriptor) {
  UniqueFd input(::fcntl(descriptor, F_DUPFD_CLOEXEC, 0));
  struct stat info {};
  if (!input || ::fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_size < 0)
    return {{}, 0, "unable to open regular file for SHA-256 verification"};
  if (static_cast<uintmax_t>(info.st_size) >
      std::numeric_limits<uint64_t>::max())
    return {{}, 0, "file is too large for SHA-256 verification"};
  if (::lseek(input.get(), 0, SEEK_SET) < 0)
    return {{}, 0, "unable to seek file for SHA-256 verification"};

  picosha2::hash256_one_by_one hasher;
  std::array<unsigned char, 64 * 1024> buffer {};
  while (true) {
    const ssize_t count = ::read(input.get(), buffer.data(), buffer.size());
    if (count > 0) {
      hasher.process(buffer.begin(), buffer.begin() + count);
    } else if (count == 0) {
      break;
    } else if (errno != EINTR) {
      return {{}, 0, "unable to read file for SHA-256 verification"};
    }
  }
  hasher.finish();
  return {picosha2::get_hash_hex_string(hasher),
          static_cast<uint64_t>(info.st_size), {}};
}

Sha256Result sha256File(const std::filesystem::path& path) {
  UniqueFd input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  return input ? sha256File(input.get())
               : Sha256Result{{}, 0,
                              "unable to open file for SHA-256 verification"};
}

}  // namespace util
