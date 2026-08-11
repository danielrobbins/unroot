#pragma once

#include <filesystem>
#include <string>
#include <sys/types.h>

#include "util/fd.hpp"

namespace util {

struct ArchiveLock {
    ArchiveLock() = default;
    ~ArchiveLock();
    ArchiveLock(const ArchiveLock&) = delete;
    ArchiveLock& operator=(const ArchiveLock&) = delete;
    ArchiveLock(ArchiveLock&& other) noexcept;
    ArchiveLock& operator=(ArchiveLock&& other) noexcept;

    UniqueFd parent;
    UniqueFd rootDescriptor;
    std::filesystem::path root;
    std::string rootName;
    std::string error;
    bool busy = false;
    bool created = false;

    explicit operator bool() const { return static_cast<bool>(rootDescriptor); }
    bool matchesRoot() const;
    std::filesystem::path pinnedRoot() const;
    void preserveRoot() { created = false; }
};

ArchiveLock acquireArchiveLock(const std::filesystem::path& root,
                               bool createRoot);

} // namespace util
