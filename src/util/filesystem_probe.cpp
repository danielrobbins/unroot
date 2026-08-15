#include "filesystem_probe.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace util {

namespace {

using ArchiveReader =
    std::unique_ptr<struct archive, decltype(&archive_read_free)>;
using ArchiveWriter =
    std::unique_ptr<struct archive, decltype(&archive_write_free)>;
using ArchiveEntry =
    std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)>;

std::string archiveMessage(struct archive* object) {
  const char* message = archive_error_string(object);
  return message ? message : "libarchive metadata operation failed";
}

fsinfo::Result probeAcl(const char* path) {
  ArchiveWriter writer(archive_write_disk_new(), archive_write_free);
  ArchiveEntry expected(archive_entry_new(), archive_entry_free);
  if (!writer || !expected)
    return {{}, "unable to allocate filesystem ACL probe"};

  archive_write_disk_set_options(
      writer.get(), ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_PERM);
  archive_entry_set_pathname(expected.get(), path);
  archive_entry_set_filetype(expected.get(), AE_IFREG);
  archive_entry_set_perm(expected.get(), 0600);
  archive_entry_set_size(expected.get(), 0);
  const int type = ARCHIVE_ENTRY_ACL_TYPE_ACCESS;
  const int subject = ::geteuid() == 1 ? 2 : 1;
  const bool validAcl =
      archive_entry_acl_add_entry(
          expected.get(), type,
          ARCHIVE_ENTRY_ACL_READ | ARCHIVE_ENTRY_ACL_WRITE,
          ARCHIVE_ENTRY_ACL_USER_OBJ, -1, nullptr) == ARCHIVE_OK &&
      archive_entry_acl_add_entry(expected.get(), type, ARCHIVE_ENTRY_ACL_READ,
                                  ARCHIVE_ENTRY_ACL_USER, subject, nullptr) ==
          ARCHIVE_OK &&
      archive_entry_acl_add_entry(expected.get(), type, 0,
                                  ARCHIVE_ENTRY_ACL_GROUP_OBJ, -1, nullptr) ==
          ARCHIVE_OK &&
      archive_entry_acl_add_entry(expected.get(), type, ARCHIVE_ENTRY_ACL_READ,
                                  ARCHIVE_ENTRY_ACL_MASK, -1, nullptr) ==
          ARCHIVE_OK &&
      archive_entry_acl_add_entry(expected.get(), type, 0,
                                  ARCHIVE_ENTRY_ACL_OTHER, -1, nullptr) ==
          ARCHIVE_OK;
  if (!validAcl) return {{}, "unable to construct filesystem ACL probe"};

  int status = archive_write_header(writer.get(), expected.get());
  if (status >= ARCHIVE_OK) status = archive_write_finish_entry(writer.get());
  if (status >= ARCHIVE_OK) status = archive_write_close(writer.get());
  if (status < ARCHIVE_OK) {
    fsinfo::FilesystemCaps caps;
    caps.posixAcl.detail = archiveMessage(writer.get());
    return {std::move(caps), {}};
  }

  ArchiveReader reader(archive_read_disk_new(), archive_read_free);
  ArchiveEntry observed(archive_entry_new(), archive_entry_free);
  if (!reader || !observed)
    return {{}, "unable to allocate filesystem ACL verification"};
  archive_read_disk_set_symlink_physical(reader.get());
  archive_entry_set_pathname(observed.get(), path);
  status = archive_read_disk_entry_from_file(reader.get(), observed.get(), -1,
                                             nullptr);
  if (status < ARCHIVE_OK) {
    fsinfo::FilesystemCaps caps;
    caps.posixAcl.detail = archiveMessage(reader.get());
    return {std::move(caps), {}};
  }

  int aclType = 0;
  int permissions = 0;
  int tag = 0;
  int qualifier = -1;
  const char* name = nullptr;
  bool found = false;
  archive_entry_acl_reset(observed.get(), type);
  while (archive_entry_acl_next(observed.get(), type, &aclType, &permissions,
                                &tag, &qualifier, &name) == ARCHIVE_OK) {
    if (aclType == type && tag == ARCHIVE_ENTRY_ACL_USER &&
        qualifier == subject && (permissions & ARCHIVE_ENTRY_ACL_READ) != 0) {
      found = true;
      break;
    }
  }
  fsinfo::FilesystemCaps caps;
  caps.posixAcl.supported = found;
  if (!found)
    caps.posixAcl.detail =
        "destination filesystem did not preserve the test POSIX ACL";
  return {std::move(caps), {}};
}

fsinfo::Result probeXattr(const char* path) {
  constexpr char Name[] = "user.unroot_probe";
  constexpr char Value[] = "unroot";
  ArchiveWriter writer(archive_write_disk_new(), archive_write_free);
  ArchiveEntry expected(archive_entry_new(), archive_entry_free);
  if (!writer || !expected)
    return {{}, "unable to allocate filesystem xattr probe"};

  archive_write_disk_set_options(
      writer.get(), ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_PERM);
  archive_entry_set_pathname(expected.get(), path);
  archive_entry_set_filetype(expected.get(), AE_IFREG);
  archive_entry_set_perm(expected.get(), 0600);
  archive_entry_set_size(expected.get(), 0);
  archive_entry_xattr_add_entry(expected.get(), Name, Value, sizeof(Value) - 1);

  int status = archive_write_header(writer.get(), expected.get());
  if (status >= ARCHIVE_OK) status = archive_write_finish_entry(writer.get());
  if (status >= ARCHIVE_OK) status = archive_write_close(writer.get());
  if (status < ARCHIVE_OK) {
    fsinfo::FilesystemCaps caps;
    caps.xattr.detail = archiveMessage(writer.get());
    return {std::move(caps), {}};
  }

  ArchiveReader reader(archive_read_disk_new(), archive_read_free);
  ArchiveEntry observed(archive_entry_new(), archive_entry_free);
  if (!reader || !observed)
    return {{}, "unable to allocate filesystem xattr verification"};
  archive_read_disk_set_symlink_physical(reader.get());
  archive_entry_set_pathname(observed.get(), path);
  status = archive_read_disk_entry_from_file(reader.get(), observed.get(), -1,
                                             nullptr);
  if (status < ARCHIVE_OK) {
    fsinfo::FilesystemCaps caps;
    caps.xattr.detail = archiveMessage(reader.get());
    return {std::move(caps), {}};
  }

  const char* name = nullptr;
  const void* value = nullptr;
  size_t size = 0;
  bool found = false;
  archive_entry_xattr_reset(observed.get());
  while (archive_entry_xattr_next(observed.get(), &name, &value, &size) ==
         ARCHIVE_OK) {
    if (name && std::strcmp(name, Name) == 0 && size == sizeof(Value) - 1 &&
        std::memcmp(value, Value, size) == 0) {
      found = true;
      break;
    }
  }
  fsinfo::FilesystemCaps caps;
  caps.xattr.supported = found;
  if (!found)
    caps.xattr.detail =
        "destination filesystem did not preserve the test extended attribute";
  return {std::move(caps), {}};
}

}  // namespace

fsinfo::Result inspectFilesystem(int descriptor) {
  struct stat info {};
  if (::fstat(descriptor, &info) != 0 || !S_ISDIR(info.st_mode))
    return {{}, "filesystem descriptor is not a directory"};
  if (::fchdir(descriptor) != 0)
    return {{}, "unable to enter destination filesystem"};

  char path[] = ".unroot-filesystem-probe-XXXXXX";
  const int probe = ::mkstemp(path);
  if (probe < 0)
    return {{}, "unable to create filesystem metadata probe: " +
                    std::string(std::strerror(errno))};
  ::close(probe);
  fsinfo::Result result = probeAcl(path);
  if (result) {
    fsinfo::Result xattr = probeXattr(path);
    if (!xattr)
      result = std::move(xattr);
    else
      result.caps.xattr = std::move(xattr.caps.xattr);
  }
  if (::unlink(path) != 0)
    return {{}, "unable to remove filesystem metadata probe: " +
                    std::string(std::strerror(errno))};
  return result;
}

}  // namespace util
