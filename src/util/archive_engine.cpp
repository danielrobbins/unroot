#include "archive_engine.hpp"

#include <string>

#include <algorithm>
#include <array>
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "archive_fd.hpp"
#include "archive_exclusions.hpp"
#include "archive_progress.hpp"

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
         std::any_of(excludes.begin(), excludes.end(), [&](const auto& item) {
           return path == item || path.rfind(item + "/", 0) == 0;
         });
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
             ArchiveProgress& progress, std::string& message) {
  static constexpr std::array<char, 64 * 1024> zeros{};
  const void* data = nullptr;
  size_t size = 0;
  la_int64_t offset = 0;
  la_int64_t position = 0;
  int status;
  while ((status = archive_read_data_block(source, &data, &size, &offset)) ==
         ARCHIVE_OK) {
    if (offset < position) {
      message = "archive disk reader returned overlapping data";
      return 1;
    }
    while (position < offset) {
      const size_t amount = static_cast<size_t>(
          std::min<la_int64_t>(offset - position, zeros.size()));
      if (!writeData(destination, zeros.data(), amount, message)) return 1;
      position += static_cast<la_int64_t>(amount);
    }
    if (!writeData(destination, data, size, message)) return 1;
    position += static_cast<la_int64_t>(size);
    progress.update(destination);
  }
  return status == ARCHIVE_EOF ? 0 : archiveFailure(source, message);
}

using FileId = std::pair<la_int64_t, la_int64_t>;
using FileLinks = std::map<FileId, std::string>;

bool rootOnlyOwnership(struct archive_entry* entry) {
  if (archive_entry_uid(entry) != 0 || archive_entry_gid(entry) != 0)
    return false;
  const int aclTypes = ARCHIVE_ENTRY_ACL_TYPE_ACCESS |
                       ARCHIVE_ENTRY_ACL_TYPE_DEFAULT;
  archive_entry_acl_reset(entry, aclTypes);
  int type, permissions, tag, qualifier;
  const char* name;
  while (archive_entry_acl_next(entry, aclTypes, &type, &permissions, &tag,
                                &qualifier, &name) == ARCHIVE_OK) {
    if ((tag == ARCHIVE_ENTRY_ACL_USER || tag == ARCHIVE_ENTRY_ACL_GROUP) &&
        qualifier != 0)
      return false;
  }
  return true;
}

bool writeEntry(struct archive* disk, struct archive* writer,
                struct archive_entry* entry, const std::string& name,
                FileLinks& links, ArchiveProgress& progress, bool force, bool rootOnly,
                std::string& message) {
  // The single-ID namespace exposes unmapped owners as overflow IDs. Never
  // publish those IDs as if they described the original rootfs ownership.
  if (rootOnly && !rootOnlyOwnership(entry)) {
    message = "--single cannot represent file or ACL ownership: " + name;
    return false;
  }
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
  if (!acceptWarning(writer, archive_write_header(writer, entry), force,
                     message) ||
      (archive_entry_size(entry) != 0 &&
       copyData(disk, writer, progress, message) != 0))
    return false;
  progress.update(writer);
  return true;
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

bool configureWriterFilter(struct archive* writer, const std::string& filter,
                           std::string& message) {
  if (filter.empty() || filter == "none") return true;
  if (archive_write_add_filter_by_name(writer, filter.c_str()) != ARCHIVE_OK) {
    archiveFailure(writer, message);
    return false;
  }
  if (filter == "xz" &&
      archive_write_set_filter_option(writer, "xz", "threads", "0") !=
          ARCHIVE_OK) {
    archiveFailure(writer, message);
    return false;
  }
  return true;
}

bool writeSubstitution(
    struct archive* writer,
    const archiveio::Substitution& substitution, FileLinks& links,
    ArchiveProgress& progress, bool force, bool rootOnly, std::string& message) {
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
                  links, progress, force, rootOnly, message)) {
    if (status == ARCHIVE_EOF && message.empty())
      message = "archive substitution source is missing";
    archive_read_close(disk.get());
    return false;
  }
  archive_read_close(disk.get());
  return true;
}

struct ExtractionProgress {
  ArchiveProgress& output;
  struct archive* reader;
};

void extractionProgress(void* data) {
  auto* progress = static_cast<ExtractionProgress*>(data);
  progress->output.update(progress->reader);
}

bool safePath(const char* raw, std::vector<std::string>& components) {
  if (!raw || !*raw || *raw == '/') return false;
  components.clear();
  const std::string path(raw);
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find('/', start);
    const std::string component = path.substr(
        start, (end == std::string::npos ? path.size() : end) - start);
    if (!component.empty() && component != ".") {
      if (component == "..") return false;
      components.push_back(component);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return !components.empty();
}

// True for a layer entry that names the root directory itself ("." or "./"),
// which image builders commonly emit. It has no components, so safePath()
// rejects it, but it is not an unsafe path: there is nothing to extract.
bool rootDirectoryEntry(const char* raw) {
  if (!raw || !*raw || *raw == '/') return false;
  const std::string path(raw);
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find('/', start);
    const std::string component = path.substr(
        start, (end == std::string::npos ? path.size() : end) - start);
    if (!component.empty() && component != ".") return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool reservedPath(const std::vector<std::string>& components) {
  return !components.empty() && components.front() == ".unroot";
}

UniqueFd openDirectory(int parent, const std::string& name) {
  return UniqueFd(::openat(parent, name.c_str(),
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
}

UniqueFd openParent(const std::vector<std::string>& components) {
  UniqueFd current(::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  for (size_t index = 0; current && index + 1 < components.size(); ++index)
    current = openDirectory(current.get(), components[index]);
  return current;
}

bool removeChild(int directory, const std::string& name) {
  struct stat info {};
  if (::fstatat(directory, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return ::unlinkat(directory, name.c_str(), 0) == 0;

  UniqueFd child = openDirectory(directory, name);
  if (!child) return errno == ENOENT;
  const int scanDescriptor = ::fcntl(child.get(), F_DUPFD_CLOEXEC, 0);
  DIR* stream = scanDescriptor >= 0 ? ::fdopendir(scanDescriptor) : nullptr;
  if (!stream) {
    if (scanDescriptor >= 0) ::close(scanDescriptor);
    return false;
  }
  std::vector<std::string> children;
  errno = 0;
  while (dirent* entry = ::readdir(stream)) {
    const std::string childName(entry->d_name);
    if (childName != "." && childName != "..")
      children.push_back(childName);
  }
  const bool listed = errno == 0;
  ::closedir(stream);
  if (!listed) return false;
  for (const auto& childName : children)
    if (!removeChild(child.get(), childName)) return false;
  child.reset();
  return ::unlinkat(directory, name.c_str(), AT_REMOVEDIR) == 0;
}

bool clearDirectory(const std::vector<std::string>& components) {
  UniqueFd directory(::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  for (const auto& component : components) {
    directory = openDirectory(directory.get(), component);
    if (!directory) return errno == ENOENT;
  }
  const int scanDescriptor = ::fcntl(directory.get(), F_DUPFD_CLOEXEC, 0);
  DIR* stream = scanDescriptor >= 0 ? ::fdopendir(scanDescriptor) : nullptr;
  if (!stream) {
    if (scanDescriptor >= 0) ::close(scanDescriptor);
    return false;
  }
  std::vector<std::string> children;
  errno = 0;
  while (dirent* entry = ::readdir(stream)) {
    const std::string name(entry->d_name);
    if (name != "." && name != ".." &&
        !(components.empty() && name == ".unroot"))
      children.push_back(name);
  }
  const bool listed = errno == 0;
  ::closedir(stream);
  if (!listed) return false;
  for (const auto& name : children)
    if (!removeChild(directory.get(), name)) return false;
  return true;
}

struct Whiteout {
  std::vector<std::string> path;
  bool opaque = false;
};

bool whiteoutEntry(struct archive_entry* entry,
                   const std::vector<std::string>& path,
                   Whiteout& whiteout, bool& marker,
                   std::string& message) {
  const std::string& name = path.back();
  marker = name.rfind(".wh.", 0) == 0;
  if (!marker) return true;
  const bool regular = archive_entry_filetype(entry) == AE_IFREG;
  const bool device = archive_entry_filetype(entry) == AE_IFCHR &&
                      archive_entry_devmajor(entry) == 0 &&
                      archive_entry_devminor(entry) == 0;
  if (!regular && !device) {
    message = "OCI whiteout marker has an invalid file type";
    return false;
  }
  whiteout.path.assign(path.begin(), path.end() - 1);
  if (name == ".wh..wh..opq") {
    whiteout.opaque = true;
    return true;
  }
  const std::string target = name.substr(4);
  if (target.empty() || target == "." || target == ".." ||
      (whiteout.path.empty() && target == ".unroot")) {
    message = "OCI whiteout marker has an invalid target";
    return false;
  }
  whiteout.path.push_back(target);
  return true;
}

bool scanAndApplyWhiteouts(int descriptor, std::string& message) {
  UniqueFd input = reopenArchiveDescriptor(descriptor, O_RDONLY, message);
  if (!input) return false;
  ArchiveReader reader(archive_read_new(), archive_read_free);
  if (!reader) {
    message = "unable to allocate OCI layer reader";
    return false;
  }
  archive_read_support_filter_all(reader.get());
  archive_read_support_format_all(reader.get());
  if (archive_read_open_fd(reader.get(), input.get(), 10240) != ARCHIVE_OK) {
    archiveFailure(reader.get(), message);
    return false;
  }

  std::vector<Whiteout> whiteouts;
  struct archive_entry* entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) !=
         ARCHIVE_EOF) {
    if (status == ARCHIVE_RETRY) continue;
    if (status < ARCHIVE_WARN) {
      archiveFailure(reader.get(), message);
      return false;
    }
    std::vector<std::string> path;
    if (rootDirectoryEntry(archive_entry_pathname(entry))) {
      if (archive_read_data_skip(reader.get()) < ARCHIVE_WARN) {
        archiveFailure(reader.get(), message);
        return false;
      }
      continue;
    }
    if (!safePath(archive_entry_pathname(entry), path) || reservedPath(path)) {
      message = "OCI layer contains an unsafe or reserved path";
      return false;
    }
    if (const char* target = archive_entry_hardlink(entry)) {
      std::vector<std::string> link;
      if (!safePath(target, link) || reservedPath(link)) {
        message = "OCI layer contains an unsafe hard-link target";
        return false;
      }
    }
    Whiteout whiteout;
    bool marker = false;
    if (!whiteoutEntry(entry, path, whiteout, marker, message)) return false;
    if (marker) whiteouts.push_back(std::move(whiteout));
    if (archive_read_data_skip(reader.get()) < ARCHIVE_WARN) {
      archiveFailure(reader.get(), message);
      return false;
    }
  }
  if (status != ARCHIVE_EOF) {
    archiveFailure(reader.get(), message);
    return false;
  }
  archive_read_close(reader.get());

  for (const auto& whiteout : whiteouts) {
    if (whiteout.opaque && !clearDirectory(whiteout.path)) {
      message = "unable to apply OCI opaque-directory marker";
      return false;
    }
  }
  for (const auto& whiteout : whiteouts) {
    if (whiteout.opaque) continue;
    UniqueFd parent = openParent(whiteout.path);
    if (!parent) {
      if (errno == ENOENT) continue;
      message = "unable to resolve OCI whiteout target";
      return false;
    }
    if (!removeChild(parent.get(), whiteout.path.back())) {
      message = "unable to apply OCI whiteout";
      return false;
    }
  }
  return true;
}

bool prepareLayerPath(struct archive_entry* entry, bool& marker,
                      std::string& message) {
  std::vector<std::string> path;
  if (rootDirectoryEntry(archive_entry_pathname(entry))) {
    marker = true;  // skipped like a whiteout marker: the root already exists
    return true;
  }
  if (!safePath(archive_entry_pathname(entry), path) || reservedPath(path)) {
    message = "OCI layer contains an unsafe or reserved path";
    return false;
  }
  Whiteout whiteout;
  if (!whiteoutEntry(entry, path, whiteout, marker, message)) return false;
  if (marker) return true;
  UniqueFd parent = openParent(path);
  if (!parent) {
    if (errno == ENOENT) return true;
    message = "unable to resolve OCI layer destination";
    return false;
  }
  struct stat existing {};
  if (::fstatat(parent.get(), path.back().c_str(), &existing,
                AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  if (archive_entry_filetype(entry) == AE_IFDIR && S_ISDIR(existing.st_mode))
    return true;
  if (!removeChild(parent.get(), path.back())) {
    message = "unable to replace a lower OCI layer path";
    return false;
  }
  return true;
}

int extractArchive(int descriptor, bool force, bool layer,
                   std::string& message, int progressDescriptor,
                   ProgressStyle progressStyle) {
  if (layer && !scanAndApplyWhiteouts(descriptor, message)) return 1;
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

  ArchiveProgress progress(progressDescriptor,
                           layer ? "Applying OCI layer" : "Extracting rootfs",
                           progressStyle, archiveDescriptorSize(input.get()));
  ExtractionProgress extraction{progress, reader.get()};
  archive_read_extract_set_progress_callback(reader.get(),
                                             extractionProgress, &extraction);

  int options =
      ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME |
      ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS | ARCHIVE_EXTRACT_XATTR |
      ARCHIVE_EXTRACT_SPARSE | ARCHIVE_EXTRACT_SECURE_SYMLINKS |
      ARCHIVE_EXTRACT_SECURE_NODOTDOT |
      ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
  if (!layer) options |= ARCHIVE_EXTRACT_NO_OVERWRITE;
  struct archive_entry* entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) !=
         ARCHIVE_EOF) {
    if (status == ARCHIVE_RETRY) continue;
    bool marker = false;
    if (!acceptWarning(reader.get(), status, force, message) ||
        (layer && !prepareLayerPath(entry, marker, message))) {
      archive_read_close(reader.get());
      return 1;
    }
    if (marker) {
      if (archive_read_data_skip(reader.get()) < ARCHIVE_WARN) {
        archiveFailure(reader.get(), message);
        archive_read_close(reader.get());
        return 1;
      }
      continue;
    }
    if (!acceptWarning(reader.get(),
                       archive_read_extract(reader.get(), entry, options),
                       force, message)) {
      archive_read_close(reader.get());
      return 1;
    }
    progress.update(reader.get());
  }
  progress.update(reader.get());
  progress.complete();
  archive_read_close(reader.get());
  return 0;
}

}  // namespace

int unpackArchive(int descriptor, bool force, std::string& message,
                   int progressDescriptor, ProgressStyle progressStyle) {
  return extractArchive(descriptor, force, false, message,
                        progressDescriptor, progressStyle);
}

int applyOciLayer(int descriptor, bool force, std::string& message,
                  int progressDescriptor, ProgressStyle progressStyle) {
  return extractArchive(descriptor, force, true, message,
                        progressDescriptor, progressStyle);
}

int packArchive(int descriptor, const std::string& filter,
                const std::vector<std::string>& excludes,
                const std::vector<std::string>& patterns,
                const std::vector<archiveio::Substitution>& substitutions,
                bool force, bool rootOnly,
                std::string& message, int progressDescriptor,
                ProgressStyle progressStyle) {
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
      !configureWriterFilter(writer.get(), filter, message) ||
      archive_write_open_fd(writer.get(), output.get()) != ARCHIVE_OK)
    return archiveFailure(writer.get(), message);
  archive_read_disk_set_symlink_physical(disk.get());
  archive_read_disk_set_behavior(disk.get(), ARCHIVE_READDISK_RESTORE_ATIME);
  if (archive_read_disk_open(disk.get(), ".") != ARCHIVE_OK) {
    return archiveFailure(disk.get(), message);
  }

  ArchiveProgress progress(progressDescriptor, "Packing rootfs",
                           progressStyle);
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
    if (excluded(name, excludes) ||
        archiveio::excludedByPattern(normalized(name), patterns)) {
      archive_entry_clear(entry.get());
      continue;
    }
    if (archive_read_disk_can_descend(disk.get()))
      archive_read_disk_descend(disk.get());
    if (!writeEntry(disk.get(), writer.get(), entry.get(),
                    name ? name : "", links, progress, force, rootOnly, message)) {
      if (message.empty()) archiveFailure(writer.get(), message);
      archive_read_close(disk.get());
      archive_write_close(writer.get());
      return 1;
    }
    archive_entry_clear(entry.get());
  }
  for (const auto& substitution : substitutions) {
    if (archiveio::excludedByPattern(substitution.destination, patterns)) continue;
    if (!writeSubstitution(writer.get(), substitution, links, progress, force, rootOnly,
                           message)) {
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
  progress.update(writer.get());
  progress.complete();
  archive_read_close(disk.get());
  return 0;
}

}  // namespace util
