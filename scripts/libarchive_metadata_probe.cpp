#include "util/filesystem_probe.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: libarchive-metadata-probe DIRECTORY\n";
    return 2;
  }

  const int descriptor = ::open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    std::cerr << "unable to open libarchive metadata probe directory: "
              << argv[1] << '\n';
    return 1;
  }

  auto result = util::inspectFilesystem(descriptor);
  ::close(descriptor);
  if (!result) {
    std::cerr << "libarchive metadata capability check failed: " << result.error
              << '\n';
    return 1;
  }

  if (result.caps.posixAcl.supported && result.caps.xattr.supported) return 0;

  std::cerr << "libarchive metadata capability check failed\n";
  if (!result.caps.posixAcl.supported)
    std::cerr << "  POSIX ACL: " << result.caps.posixAcl.detail << '\n';
  if (!result.caps.xattr.supported)
    std::cerr << "  xattr: " << result.caps.xattr.detail << '\n';
  std::cerr << "Ensure libarchive was built with ACL and xattr support and "
               "that "
            << argv[1] << " supports both metadata types.\n";
  return 1;
}
