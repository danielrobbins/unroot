#pragma once

#include <fcntl.h>
#include <string>
#include <sys/stat.h>

#include "util/fd.hpp"

namespace util {

class Rootfs {
 public:
  explicit Rootfs(const std::string& path);

  explicit operator bool() const { return static_cast<bool>(root_); }
  int fd() const { return root_.get(); }

  UniqueFd directory(const std::string& path, bool create = false,
                     mode_t mode = 0755) const;
  UniqueFd file(const std::string& path, int flags, mode_t mode = 0644,
                bool createParents = false) const;
  UniqueFd resolvedFile(const std::string& path, int flags = O_RDONLY) const;
  UniqueFd mountTarget(const std::string& path, bool directory) const;

  bool readText(const std::string& path, std::string& content) const;
  bool writeTextAtomic(const std::string& path, const std::string& content,
                       mode_t mode = 0644) const;
  bool copyHostFileAtomic(const std::string& source,
                          const std::string& destination,
                          mode_t mode = 0755) const;
  bool linkHostFile(const std::string& source,
                    const std::string& destination) const;
  bool stat(const std::string& path, struct stat& result) const;

  static std::string fdPath(int fd);

 private:
  UniqueFd parent(const std::string& path, bool create,
                  std::string& leaf) const;
  UniqueFd temporary(int parent, const std::string& leaf, mode_t mode,
                     std::string& name) const;
  static bool replace(int parent, const std::string& temporary,
                      const std::string& destination, UniqueFd output,
                      bool ready, mode_t mode);

  UniqueFd root_;
};

}  // namespace util
