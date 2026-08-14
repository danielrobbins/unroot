#include "archive_engine.hpp"

#include <string>

#ifdef UNROOT_HAVE_LIBARCHIVE
#include <algorithm>
#include <array>
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <map>
#include <memory>

#include "archive_fd.hpp"
#endif

namespace util {

#ifdef UNROOT_HAVE_LIBARCHIVE
namespace {

using ArchiveReader =
    std::unique_ptr<struct archive, decltype(&archive_read_free)>;
using ArchiveWriter =
    std::unique_ptr<struct archive, decltype(&archive_write_free)>;
using ArchiveEntry =
    std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)>;

std::string archiveMessage(struct archive* object) {
  const char* message = archive_error_string(object);
  return message ? message : "libarchive operation failed";
}

int archiveFailure(struct archive* object, std::string& error) {
  error = archiveMessage(object);
  return 1;
}

bool acceptWarning(struct archive* object, int status, bool force,
                   std::string& message) {
  if (status >= ARCHIVE_OK) return true;
  const std::string detail = archiveMessage(object);
  if (status < ARCHIVE_WARN || !force) {
    message = detail;
    if (status == ARCHIVE_WARN)
      message += "; use --force to accept metadata loss";
    return false;
  }
  if (message.empty()) message = detail;
  return true;
}

std::string normalized(const char* name) {
  if (!name) return {};
  std::string path(name);
  while (path.rfind("./", 0) == 0) path.erase(0, 2);
  return path;
}

bool excluded(const char* name, const std::vector<std::string>& excludes) {
  const std::string path = normalized(name);
  return path == ".unroot" || path.rfind(".unroot/", 0) == 0 ||
         std::find(excludes.begin(), excludes.end(), path) != excludes.end();
}

bool writeData(struct archive* destination, const void* data, size_t size,
               std::string& message) {
  const auto written = archive_write_data(destination, data, size);
  if (written < 0) {
    archiveFailure(destination, message);
    return false;
  }
  if (static_cast<size_t>(written) != size) {
    message = "archive data write was truncated";
    return false;
  }
  return true;
}

int copyData(struct archive* source, struct archive* destination,
             std::string& message) {
  static constexpr std::array<char, 64 * 1024> zeros{};
  const void* data = nullptr;
  size_t size = 0;
  la_int64_t offset = 0;
  la_int64_t progress = 0;
  int status;
  while ((status = archive_read_data_block(source, &data, &size, &offset)) ==
         ARCHIVE_OK) {
    if (offset < progress) {
      message = "archive disk reader returned overlapping data";
      return 1;
    }
    while (progress < offset) {
      const size_t amount = static_cast<size_t>(
          std::min<la_int64_t>(offset - progress, zeros.size()));
      if (!writeData(destination, zeros.data(), amount, message)) return 1;
      progress += static_cast<la_int64_t>(amount);
    }
    if (!writeData(destination, data, size, message)) return 1;
    progress += static_cast<la_int64_t>(size);
  }
  return status == ARCHIVE_EOF ? 0 : archiveFailure(source, message);
}

using FileId = std::pair<la_int64_t, la_int64_t>;
using FileLinks = std::map<FileId, std::string>;

bool writeEntry(struct archive* disk, struct archive* writer,
                struct archive_entry* entry, const std::string& name,
                FileLinks& links, bool force, std::string& message) {
  archive_entry_set_pathname(entry, name.c_str());
  if (archive_entry_filetype(entry) != AE_IFREG)
    archive_entry_set_size(entry, 0);
  else if (archive_entry_nlink(entry) > 1) {
    const FileId id{archive_entry_dev(entry), archive_entry_ino64(entry)};
    auto [found, inserted] = links.emplace(id, name);
    if (!inserted) {
      archive_entry_set_hardlink(entry, found->second.c_str());
      archive_entry_set_size(entry, 0);
    }
  }
  return acceptWarning(writer, archive_write_header(writer, entry), force,
                       message) &&
         (archive_entry_size(entry) == 0 ||
          copyData(disk, writer, message) == 0);
}

bool validRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/') return false;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find('/', start);
    const std::string component = path.substr(
        start, (end == std::string::npos ? path.size() : end) - start);
    if (component.empty() || component == "." || component == "..")
      return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool writeSubstitution(
    struct archive* writer,
    const archiveio::Substitution& substitution, FileLinks& links,
    bool force, std::string& message) {
  if (!validRelativePath(substitution.source) ||
      !validRelativePath(substitution.destination)) {
    message = "invalid archive substitution path";
    return false;
  }
  ArchiveReader disk(archive_read_disk_new(), archive_read_free);
  ArchiveEntry entry(archive_entry_new(), archive_entry_free);
  if (!disk || !entry) {
    message = "unable to allocate archive substitution reader";
    return false;
  }
  archive_read_disk_set_symlink_physical(disk.get());
  archive_read_disk_set_behavior(disk.get(), ARCHIVE_READDISK_RESTORE_ATIME);
  if (archive_read_disk_open(disk.get(), substitution.source.c_str()) !=
      ARCHIVE_OK) {
    archiveFailure(disk.get(), message);
    return false;
  }
  const int status = archive_read_next_header2(disk.get(), entry.get());
  if (!acceptWarning(disk.get(), status, force, message) ||
      status == ARCHIVE_EOF ||
      !writeEntry(disk.get(), writer, entry.get(), substitution.destination,
                  links, force, message)) {
    if (status == ARCHIVE_EOF && message.empty())
      message = "archive substitution source is missing";
    archive_read_close(disk.get());
    return false;
  }
  archive_read_close(disk.get());
  return true;
}

}  // namespace

int unpackArchive(int descriptor, bool force, std::string& message) {
  UniqueFd input = reopenArchiveDescriptor(descriptor, O_RDONLY, message);
  if (!input) return 1;
  ArchiveReader reader(archive_read_new(), archive_read_free);
  if (!reader) {
    message = "unable to allocate libarchive reader";
    return 1;
  }
  archive_read_support_filter_all(reader.get());
  archive_read_support_format_all(reader.get());
  if (archive_read_open_fd(reader.get(), input.get(), 10240) != ARCHIVE_OK)
    return archiveFailure(reader.get(), message);

  constexpr int options =
      ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME |
      ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_XATTR |
      ARCHIVE_EXTRACT_SPARSE | ARCHIVE_EXTRACT_NO_OVERWRITE |
      ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
      ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
  struct archive_entry* entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) !=
         ARCHIVE_EOF) {
    if (status == ARCHIVE_RETRY) continue;
    if (!acceptWarning(reader.get(), status, force, message) ||
        !acceptWarning(reader.get(),
                       archive_read_extract(reader.get(), entry, options),
                       force, message)) {
      archive_read_close(reader.get());
      return 1;
    }
  }
  archive_read_close(reader.get());
  return 0;
}

int packArchive(int descriptor, const std::string& filter,
                const std::vector<std::string>& excludes,
                const std::vector<archiveio::Substitution>& substitutions,
                bool force,
                std::string& message) {
  UniqueFd output =
      reopenArchiveDescriptor(descriptor, O_WRONLY | O_TRUNC, message);
  if (!output) return 1;
  ArchiveWriter writer(archive_write_new(), archive_write_free);
  ArchiveReader disk(archive_read_disk_new(), archive_read_free);
  if (!writer || !disk) {
    message = "unable to allocate libarchive writer";
    return 1;
  }
  if (archive_write_set_format_pax(writer.get()) != ARCHIVE_OK ||
      (!filter.empty() && filter != "none" &&
       archive_write_add_filter_by_name(writer.get(), filter.c_str()) !=
           ARCHIVE_OK) ||
      archive_write_open_fd(writer.get(), output.get()) != ARCHIVE_OK)
    return archiveFailure(writer.get(), message);
  archive_read_disk_set_symlink_physical(disk.get());
  archive_read_disk_set_behavior(disk.get(), ARCHIVE_READDISK_RESTORE_ATIME);
  if (archive_read_disk_open(disk.get(), ".") != ARCHIVE_OK) {
    return archiveFailure(disk.get(), message);
  }

  FileLinks links;
  ArchiveEntry entry(archive_entry_new(), archive_entry_free);
  int status;
  while ((status = archive_read_next_header2(disk.get(), entry.get())) !=
         ARCHIVE_EOF) {
    if (status == ARCHIVE_RETRY) continue;
    if (!acceptWarning(disk.get(), status, force, message)) {
      archive_read_close(disk.get());
      archive_write_close(writer.get());
      return 1;
    }
    const char* name = archive_entry_pathname(entry.get());
    if (excluded(name, excludes)) {
      archive_entry_clear(entry.get());
      continue;
    }
    if (archive_read_disk_can_descend(disk.get()))
      archive_read_disk_descend(disk.get());
    if (!writeEntry(disk.get(), writer.get(), entry.get(),
                    name ? name : "", links, force, message)) {
      if (message.empty()) archiveFailure(writer.get(), message);
      archive_read_close(disk.get());
      archive_write_close(writer.get());
      return 1;
    }
    archive_entry_clear(entry.get());
  }
  for (const auto& substitution : substitutions) {
    if (!writeSubstitution(writer.get(), substitution, links, force, message)) {
      archive_read_close(disk.get());
      archive_write_close(writer.get());
      return 1;
    }
  }
  const int closeStatus = archive_write_close(writer.get());
  if (!acceptWarning(writer.get(), closeStatus, force, message)) {
    const int result = 1;
    archive_read_close(disk.get());
    return result;
  }
  archive_read_close(disk.get());
  return 0;
}

#else

int unpackArchive(int, bool, std::string& message) {
  message = "archive extraction requires libarchive support";
  return 1;
}

int packArchive(int, const std::string&, const std::vector<std::string>&,
                const std::vector<archiveio::Substitution>&, bool,
                std::string& message) {
  message = "archive creation requires libarchive support";
  return 1;
}

#endif

}  // namespace util
