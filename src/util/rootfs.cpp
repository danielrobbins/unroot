#include "util/rootfs.hpp"

#include <cerrno>
#include <deque>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "util/io.hpp"

namespace util {
namespace {

bool splitPath(const std::string& path, std::vector<std::string>& parts) {
  size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/') ++start;
    if (start == path.size()) break;
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    std::string part = path.substr(start, end - start);
    if (part == "..") {
      errno = EINVAL;
      return false;
    }
    if (part != ".") parts.push_back(std::move(part));
    start = end;
  }
  return true;
}

void prependPath(const std::string& path, std::deque<std::string>& pending) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/') ++start;
    if (start == path.size()) break;
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    parts.push_back(path.substr(start, end - start));
    start = end;
  }
  for (auto part = parts.rbegin(); part != parts.rend(); ++part)
    pending.push_front(std::move(*part));
}

UniqueFd duplicate(int fd) {
  return UniqueFd(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
}

UniqueFd openDirectoryAt(int parent, const std::string& name) {
  return UniqueFd(::openat(parent, name.c_str(),
                           O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
}

}  // namespace

Rootfs::Rootfs(const std::string& path)
    : root_(::open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)) {}

UniqueFd Rootfs::directory(const std::string& path, bool create,
                           mode_t mode) const {
  if (!root_) {
    errno = EBADF;
    return {};
  }
  std::vector<std::string> parts;
  if (!splitPath(path, parts)) return {};

  UniqueFd current = duplicate(root_.get());
  for (const auto& part : parts) {
    UniqueFd next = openDirectoryAt(current.get(), part);
    if (!next && create && errno == ENOENT) {
      if (::mkdirat(current.get(), part.c_str(), mode) != 0 && errno != EEXIST)
        return {};
      next = openDirectoryAt(current.get(), part);
    }
    if (!next) return {};
    current = std::move(next);
  }
  return current;
}

UniqueFd Rootfs::parent(const std::string& path, bool create,
                        std::string& leaf) const {
  std::vector<std::string> parts;
  if (!splitPath(path, parts) || parts.empty()) {
    errno = EINVAL;
    return {};
  }
  leaf = std::move(parts.back());
  parts.pop_back();

  std::string parentPath;
  for (const auto& part : parts) parentPath += "/" + part;
  return directory(parentPath, create);
}

UniqueFd Rootfs::file(const std::string& path, int flags, mode_t mode,
                      bool createParents) const {
  std::string leaf;
  UniqueFd dir = parent(path, createParents, leaf);
  if (!dir) return {};
  return UniqueFd(::openat(dir.get(), leaf.c_str(),
                           flags | O_NOFOLLOW | O_CLOEXEC, mode));
}

UniqueFd Rootfs::resolvedFile(const std::string& path, int flags) const {
  if (!root_ || path.empty()) {
    errno = EINVAL;
    return {};
  }

  std::deque<std::string> pending;
  prependPath(path, pending);
  std::vector<UniqueFd> directories;
  directories.push_back(duplicate(root_.get()));
  unsigned int links = 0;

  while (!pending.empty()) {
    std::string part = std::move(pending.front());
    pending.pop_front();
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (directories.size() > 1) directories.pop_back();
      continue;
    }

    struct stat info {};
    int parentFd = directories.back().get();
    if (::fstatat(parentFd, part.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
      return {};
    if (S_ISLNK(info.st_mode)) {
      if (++links > 40) {
        errno = ELOOP;
        return {};
      }
      std::vector<char> target(static_cast<size_t>(info.st_size) + 256);
      ssize_t count = ::readlinkat(parentFd, part.c_str(), target.data(),
                                   target.size());
      if (count < 0 || static_cast<size_t>(count) == target.size()) {
        errno = count < 0 ? errno : ENAMETOOLONG;
        return {};
      }
      std::string link(target.data(), static_cast<size_t>(count));
      if (!link.empty() && link.front() == '/') {
        directories.resize(1);
      }
      prependPath(link, pending);
      continue;
    }

    if (pending.empty()) {
      return UniqueFd(::openat(parentFd, part.c_str(),
                               flags | O_NOFOLLOW | O_CLOEXEC));
    }
    if (!S_ISDIR(info.st_mode)) {
      errno = ENOTDIR;
      return {};
    }
    UniqueFd next = openDirectoryAt(parentFd, part);
    if (!next) return {};
    directories.push_back(std::move(next));
  }

  errno = EISDIR;
  return {};
}

UniqueFd Rootfs::mountTarget(const std::string& path, bool isDirectory) const {
  if (isDirectory) return directory(path, true);

  UniqueFd target = file(path, O_PATH, 0, true);
  if (!target && errno == ENOENT) {
    UniqueFd created = file(path, O_RDONLY | O_CREAT | O_EXCL, 0644, true);
    if (!created) return {};
    target = file(path, O_PATH, 0, false);
  }
  if (!target) return {};

  struct stat info {};
  if (::fstat(target.get(), &info) != 0 || S_ISLNK(info.st_mode) ||
      S_ISDIR(info.st_mode)) {
    errno = EINVAL;
    return {};
  }
  return target;
}

bool Rootfs::readText(const std::string& path, std::string& content) const {
  UniqueFd input = file(path, O_RDONLY);
  if (!input) return false;

  content.clear();
  char buffer[4096];
  while (true) {
    ssize_t count = ::read(input.get(), buffer, sizeof(buffer));
    if (count > 0) {
      content.append(buffer, static_cast<size_t>(count));
    } else if (count == 0) {
      return true;
    } else if (errno != EINTR) {
      return false;
    }
  }
}

bool Rootfs::writeTextAtomic(const std::string& path,
                             const std::string& content, mode_t mode) const {
  std::string leaf;
  UniqueFd dir = parent(path, true, leaf);
  if (!dir) return false;

  std::string temporaryName;
  UniqueFd output = temporary(dir.get(), leaf, mode, temporaryName);
  if (!output) return false;
  bool written = output &&
                 util::write_all(output.get(), content.data(), content.size());
  return replace(dir.get(), temporaryName, leaf, std::move(output), written,
                 mode);
}

bool Rootfs::copyHostFileAtomic(const std::string& source,
                                const std::string& destination,
                                mode_t mode) const {
  UniqueFd input(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
  std::string leaf;
  UniqueFd dir = parent(destination, true, leaf);
  if (!input || !dir) return false;

  std::string temporaryName;
  UniqueFd output = temporary(dir.get(), leaf, mode, temporaryName);
  if (!output) return false;

  char buffer[65536];
  ssize_t count;
  bool copied = true;
  while (copied && (count = ::read(input.get(), buffer, sizeof(buffer))) > 0)
    copied = util::write_all(output.get(), buffer, static_cast<size_t>(count));
  if (count < 0) copied = false;
  return replace(dir.get(), temporaryName, leaf, std::move(output), copied,
                 mode);
}

UniqueFd Rootfs::temporary(int parentFd, const std::string& leaf, mode_t mode,
                           std::string& name) const {
  UniqueFd output;
  for (unsigned int attempt = 0; attempt < 100 && !output; ++attempt) {
    name = "." + leaf + ".tmp." + std::to_string(::getpid()) + "." +
           std::to_string(attempt);
    output.reset(::openat(parentFd, name.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                          mode));
    if (!output && errno != EEXIST) return {};
  }
  return output;
}

bool Rootfs::replace(int parentFd, const std::string& temporary,
                     const std::string& destination, UniqueFd output,
                     bool ready, mode_t mode) {
  if (ready) ready = output && ::fchmod(output.get(), mode) == 0;
  output.reset();
  if (ready)
    ready = ::renameat(parentFd, temporary.c_str(), parentFd,
                       destination.c_str()) == 0;
  if (!ready) (void)::unlinkat(parentFd, temporary.c_str(), 0);
  return ready;
}

bool Rootfs::linkHostFile(const std::string& source,
                          const std::string& destination) const {
  std::string leaf;
  UniqueFd dir = parent(destination, true, leaf);
  return dir && ::linkat(AT_FDCWD, source.c_str(), dir.get(), leaf.c_str(), 0) ==
                    0;
}

bool Rootfs::stat(const std::string& path, struct stat& result) const {
  UniqueFd target = file(path, O_PATH);
  return target && ::fstat(target.get(), &result) == 0 &&
         !S_ISLNK(result.st_mode);
}

std::string Rootfs::fdPath(int fd) {
  return "/proc/self/fd/" + std::to_string(fd);
}

}  // namespace util
