#include "util/archive_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace util {
namespace {

ArchiveLock failure(std::string error, bool busy = false) {
    ArchiveLock result;
    result.error = std::move(error);
    result.busy = busy;
    return result;
}

bool sameFile(int descriptor, int parent, const std::string& name) {
    struct stat opened{};
    struct stat linked{};
    return ::fstat(descriptor, &opened) == 0 &&
           ::fstatat(parent, name.c_str(), &linked, AT_SYMLINK_NOFOLLOW) == 0 &&
           opened.st_dev == linked.st_dev && opened.st_ino == linked.st_ino;
}

void releaseRoot(ArchiveLock& lock) {
    if (lock.created && lock.parent && lock.rootDescriptor &&
        !lock.rootName.empty() &&
        sameFile(lock.rootDescriptor.get(), lock.parent.get(), lock.rootName))
        (void)::unlinkat(lock.parent.get(), lock.rootName.c_str(), AT_REMOVEDIR);
    lock.created = false;
    lock.rootDescriptor.reset();
}

} // namespace

ArchiveLock::~ArchiveLock() {
    releaseRoot(*this);
}

ArchiveLock::ArchiveLock(ArchiveLock&& other) noexcept
    : parent(std::move(other.parent)),
      rootDescriptor(std::move(other.rootDescriptor)),
      root(std::move(other.root)),
      rootName(std::move(other.rootName)),
      error(std::move(other.error)),
      busy(other.busy),
      created(std::exchange(other.created, false)) {}

ArchiveLock& ArchiveLock::operator=(ArchiveLock&& other) noexcept {
    if (this == &other) return *this;
    releaseRoot(*this);
    parent = std::move(other.parent);
    rootDescriptor = std::move(other.rootDescriptor);
    root = std::move(other.root);
    rootName = std::move(other.rootName);
    error = std::move(other.error);
    busy = other.busy;
    created = std::exchange(other.created, false);
    return *this;
}

ArchiveLock acquireArchiveLock(const std::filesystem::path& rootPath,
                               bool createRoot) {
    if (createRoot) {
        std::error_code error;
        std::filesystem::create_directories(rootPath.parent_path(), error);
        if (error)
            return failure("unable to prepare ROOT parent directory: " +
                           error.message());
    }
    UniqueFd parent(::open(rootPath.parent_path().c_str(),
                           O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!parent) return failure("unable to open ROOT parent directory");
    const std::string rootName = rootPath.filename().string();
    bool created = false;
    if (createRoot) {
        if (::mkdirat(parent.get(), rootName.c_str(), 0755) == 0)
            created = true;
        else if (errno != EEXIST)
            return failure("unable to create ROOT under archive lock");
    }
    UniqueFd rootDescriptor(::openat(parent.get(), rootName.c_str(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                         O_CLOEXEC));
    if (!rootDescriptor) return failure("unable to open ROOT for archive lock");
    ArchiveLock result;
    result.parent = std::move(parent);
    result.rootDescriptor = std::move(rootDescriptor);
    result.root = rootPath;
    result.rootName = rootName;
    result.created = created;
    int locked;
    do {
        locked = ::flock(result.rootDescriptor.get(), LOCK_EX | LOCK_NB);
    } while (locked != 0 && errno == EINTR);
    if (locked != 0) {
        if (errno == EACCES || errno == EAGAIN)
            return failure("archive operation is already active for ROOT", true);
        return failure("unable to lock rootfs archive operations");
    }

    if (!result.matchesRoot())
        return failure("ROOT changed during archive lock acquisition");
    return result;
}

bool ArchiveLock::matchesRoot() const {
    return parent && rootDescriptor &&
           sameFile(rootDescriptor.get(), parent.get(), rootName) &&
           sameFile(rootDescriptor.get(), AT_FDCWD, root.string());
}

std::filesystem::path ArchiveLock::pinnedRoot() const {
    return rootDescriptor ? std::filesystem::path("/proc/self/fd") /
                                std::to_string(rootDescriptor.get())
                          : std::filesystem::path{};
}

} // namespace util
