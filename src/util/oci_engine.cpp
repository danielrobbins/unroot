#include "oci_engine.hpp"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "archive_fd.hpp"
#include "util/io.hpp"
#include "util/rootfs.hpp"

namespace util {
namespace {

using Reader = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

std::string archiveMessage(struct archive* object) {
  const char* message = archive_error_string(object);
  return message ? message : "libarchive operation failed";
}

bool normalizedPath(const char* raw, std::string& path) {
  if (!raw || !*raw || *raw == '/') return false;
  path.clear();
  const std::string input(raw);
  size_t start = 0;
  while (start <= input.size()) {
    const size_t end = input.find('/', start);
    const std::string component = input.substr(
        start, (end == std::string::npos ? input.size() : end) - start);
    if (!component.empty() && component != ".") {
      if (component == "..") return false;
      if (!path.empty()) path.push_back('/');
      path += component;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (path.empty()) path = ".";
  return true;
}

bool lowerHex(const std::string& value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool stagedFile(const std::string& path) {
  if (path == "oci-layout" || path == "index.json") return true;
  constexpr const char* prefix = "blobs/sha256/";
  return path.rfind(prefix, 0) == 0 &&
         path.size() == std::char_traits<char>::length(prefix) + 64 &&
         lowerHex(path.substr(std::char_traits<char>::length(prefix)));
}

bool copyEntry(struct archive* reader, int output, std::string& error) {
  std::array<char, 64 * 1024> buffer {};
  while (true) {
    const la_ssize_t count = archive_read_data(reader, buffer.data(),
                                               buffer.size());
    if (count > 0) {
      if (!write_all(output, buffer.data(), static_cast<size_t>(count))) {
        error = "unable to write staged OCI content";
        return false;
      }
    } else if (count == 0) {
      return true;
    } else {
      error = archiveMessage(reader);
      return false;
    }
  }
}

Reader openReader(int descriptor, bool raw, UniqueFd& input,
                  std::string& error) {
  input = reopenArchiveDescriptor(descriptor, O_RDONLY, error);
  Reader reader(archive_read_new(), archive_read_free);
  if (!input || !reader) {
    if (error.empty()) error = "unable to allocate libarchive reader";
    return {nullptr, archive_read_free};
  }
  archive_read_support_filter_all(reader.get());
  if (raw)
    archive_read_support_format_raw(reader.get());
  else
    archive_read_support_format_all(reader.get());
  if (archive_read_open_fd(reader.get(), input.get(), 10240) != ARCHIVE_OK) {
    error = archiveMessage(reader.get());
    return {nullptr, archive_read_free};
  }
  return reader;
}

std::string activeFilter(struct archive* reader) {
  for (int index = 0; index < archive_filter_count(reader); ++index) {
    const char* name = archive_filter_name(reader, index);
    if (name && std::string(name) != "none") return name;
  }
  return "none";
}

}  // namespace

int stageOciLayout(int descriptor, std::string& error) {
  UniqueFd input;
  Reader reader = openReader(descriptor, false, input, error);
  if (!reader) return 1;
  Rootfs destination(".");
  if (!destination) {
    error = "unable to open OCI staging directory";
    return 1;
  }

  bool layout = false;
  bool index = false;
  struct archive_entry* entry = nullptr;
  int status;
  while ((status = archive_read_next_header(reader.get(), &entry)) !=
         ARCHIVE_EOF) {
    if (status == ARCHIVE_RETRY) continue;
    if (status < ARCHIVE_WARN) {
      error = archiveMessage(reader.get());
      return 1;
    }
    std::string path;
    if (!normalizedPath(archive_entry_pathname(entry), path)) {
      error = "OCI layout contains an unsafe archive path";
      return 1;
    }
    if (!stagedFile(path)) {
      if (archive_read_data_skip(reader.get()) < ARCHIVE_WARN) {
        error = archiveMessage(reader.get());
        return 1;
      }
      continue;
    }
    if (archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_hardlink(entry) || archive_entry_symlink(entry)) {
      error = "OCI layout files and blobs must be regular files";
      return 1;
    }
    UniqueFd output = destination.file(
        path, O_WRONLY | O_CREAT | O_EXCL, 0600, true);
    if (!output) {
      error = errno == EEXIST ? "OCI layout contains a duplicate file: " + path
                              : "unable to stage OCI layout file: " + path;
      return 1;
    }
    if (!copyEntry(reader.get(), output.get(), error) ||
        ::fsync(output.get()) != 0) {
      if (error.empty()) error = "unable to sync staged OCI content";
      return 1;
    }
    layout = layout || path == "oci-layout";
    index = index || path == "index.json";
  }
  if (status != ARCHIVE_EOF) {
    error = archiveMessage(reader.get());
    return 1;
  }
  if (!layout || !index) {
    error = "OCI image archive must contain oci-layout and index.json";
    return 1;
  }
  return 0;
}

int decompressOciLayer(int descriptor, int outputDescriptor,
                       const std::string& expectedFilter,
                       std::string& error) {
  UniqueFd input;
  Reader reader = openReader(descriptor, true, input, error);
  if (!reader) return 1;
  struct archive_entry* entry = nullptr;
  if (archive_read_next_header(reader.get(), &entry) != ARCHIVE_OK) {
    error = archiveMessage(reader.get());
    return 1;
  }
  const std::string filter = activeFilter(reader.get());
  if (filter != expectedFilter) {
    error = "OCI layer declares " + expectedFilter +
            " compression but contains " + filter;
    return 1;
  }
  UniqueFd output = reopenArchiveDescriptor(outputDescriptor,
                                             O_WRONLY | O_TRUNC, error);
  if (!output) return 1;
  if (!copyEntry(reader.get(), output.get(), error) ||
      ::fsync(output.get()) != 0) {
    if (error.empty()) error = "unable to sync decompressed OCI layer";
    return 1;
  }
  if (archive_read_next_header(reader.get(), &entry) != ARCHIVE_EOF) {
    error = "OCI layer compression stream contains trailing entries";
    return 1;
  }
  return 0;
}

}  // namespace util
