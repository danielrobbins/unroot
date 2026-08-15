#include "archive_inspector.hpp"

#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "archive_fd.hpp"
#include "archive_progress.hpp"

namespace util {

namespace {

using Reader = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

void addExample(archiveinfo::Finding& finding, const std::string& path,
                uint64_t amount = 1) {
  finding.count += amount;
  if (finding.examples.size() < 4 &&
      std::find(finding.examples.begin(), finding.examples.end(), path) ==
          finding.examples.end())
    finding.examples.push_back(path.substr(0, 512));
}

struct NormalizedPath {
  std::string value;
  std::string top;
  bool unsafe = false;
};

NormalizedPath normalizePath(const char* raw) {
  if (!raw) return {{}, {}, true};
  const std::string input(raw);
  NormalizedPath result;
  result.unsafe = !input.empty() && input.front() == '/';
  std::vector<std::string> components;
  size_t begin = 0;
  while (begin <= input.size()) {
    const size_t end = input.find('/', begin);
    const std::string component =
        input.substr(begin, end == std::string::npos ? end : end - begin);
    if (!component.empty() && component != ".") {
      if (component == "..") {
        if (components.empty())
          result.unsafe = true;
        else
          components.pop_back();
      } else {
        components.push_back(component);
      }
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  if (!components.empty()) result.top = components.front();
  for (const auto& component : components) {
    if (!result.value.empty()) result.value.push_back('/');
    result.value += component;
  }
  if (result.value.empty()) result.value = ".";
  return result;
}

void inspectMetadata(struct archive_entry* entry, const std::string& path,
                     archiveinfo::ArchiveReport& report) {
  const int access = archive_entry_acl_count(entry, ARCHIVE_ENTRY_ACL_TYPE_ACCESS);
  const int defaults = archive_entry_acl_count(entry, ARCHIVE_ENTRY_ACL_TYPE_DEFAULT);
  if (access > 3 || defaults > 0) addExample(report.acls, path);

  const int xattrs = archive_entry_xattr_count(entry);
  if (xattrs > 0) {
    addExample(report.xattrs, path, static_cast<uint64_t>(xattrs));
    archive_entry_xattr_reset(entry);
    const char* name = nullptr;
    const void* value = nullptr;
    size_t size = 0;
    while (archive_entry_xattr_next(entry, &name, &value, &size) == ARCHIVE_OK) {
      (void)value;
      (void)size;
      if (!name) continue;
      if (std::string(name) == "security.capability")
        addExample(report.capabilities, path);
      else if (std::string(name) == "security.selinux")
        addExample(report.selinuxLabels, path);
    }
  }
  if (archive_entry_sparse_count(entry) > 0)
    addExample(report.sparseFiles, path);
}

void inspectType(struct archive_entry* entry, const std::string& path,
                  archiveinfo::ArchiveReport& report) {
  if (const char* target = archive_entry_hardlink(entry)) {
    addExample(report.hardLinks, path);
    const auto normalized = normalizePath(target);
    if (normalized.unsafe)
      addExample(report.unsafePaths, path + " -> " + normalized.value);
    if (normalized.top == ".unroot") report.reservedMetadata = true;
  }
  switch (archive_entry_filetype(entry)) {
    case AE_IFLNK:
      addExample(report.symbolicLinks, path);
      break;
    case AE_IFCHR:
    case AE_IFBLK:
      addExample(report.devices, path);
      break;
    case AE_IFIFO:
      addExample(report.fifos, path);
      break;
    case AE_IFSOCK:
      addExample(report.sockets, path);
      break;
    default:
      break;
  }
}

}  // namespace

ArchiveScanResult inspectArchive(int descriptor, int progressDescriptor,
                                 ProgressStyle progressStyle) {
  std::string error;
  UniqueFd input = reopenArchiveDescriptor(descriptor, O_RDONLY, error);
  if (!input) return {{}, std::move(error)};

  Reader reader(archive_read_new(), archive_read_free);
  if (!reader) {
    return {{}, "unable to allocate libarchive reader"};
  }
  archive_read_support_filter_all(reader.get());
  archive_read_support_format_all(reader.get());
  if (archive_read_open_fd(reader.get(), input.get(), 10240) != ARCHIVE_OK) {
    error = archive_error_string(reader.get())
                ? archive_error_string(reader.get())
                : "unable to open archive";
    return {{}, std::move(error)};
  }

  ArchiveProgress progress(progressDescriptor, "Inspecting archive",
                           progressStyle, archiveDescriptorSize(input.get()));
  archiveinfo::ArchiveReport report;
  struct archive_entry* entry = nullptr;
  int status = ARCHIVE_OK;
  while ((status = archive_read_next_header(reader.get(), &entry)) ==
         ARCHIVE_OK) {
    const auto path = normalizePath(archive_entry_pathname(entry));
    ++report.members;
    if (path.unsafe) addExample(report.unsafePaths, path.value);
    if (path.top == ".unroot") report.reservedMetadata = true;
    if (path.top == "oci-layout" && path.value == "oci-layout")
      report.ociLayout = true;

    const auto uid = archive_entry_uid(entry);
    const auto gid = archive_entry_gid(entry);
    if (uid >= 0)
      report.maxUid =
          std::max(report.maxUid, static_cast<uint64_t>(uid));
    if (gid >= 0)
      report.maxGid =
          std::max(report.maxGid, static_cast<uint64_t>(gid));
    if (archive_entry_filetype(entry) == AE_IFREG &&
        archive_entry_size_is_set(entry)) {
      const auto size = archive_entry_size(entry);
      if (size > 0 && report.regularBytes <=
                          std::numeric_limits<uint64_t>::max() -
                              static_cast<uint64_t>(size))
        report.regularBytes += static_cast<uint64_t>(size);
    }
    inspectMetadata(entry, path.value, report);
    inspectType(entry, path.value, report);
    if (archive_read_data_skip(reader.get()) < ARCHIVE_WARN) break;
    progress.update(reader.get());
  }

  if (const char* name = archive_format_name(reader.get())) report.format = name;
  for (int index = 0; index < archive_filter_count(reader.get()); ++index) {
    const char* name = archive_filter_name(reader.get(), index);
    if (name && std::string(name) != "none") report.filters.emplace_back(name);
  }
  if (status != ARCHIVE_EOF) {
    error = archive_error_string(reader.get())
                ? archive_error_string(reader.get())
                : "unable to read archive";
    archive_read_close(reader.get());
    return {{}, std::move(error)};
  }
  progress.update(reader.get());
  progress.complete();
  archive_read_close(reader.get());
  return {std::move(report), {}};
}

std::string archiveLibraryVersion() {
  const char* version = archive_version_string();
  return version ? version : "libarchive";
}

}  // namespace util
