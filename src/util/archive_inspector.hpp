#pragma once

#include <string>

#include "archive_report.hpp"
#include "util/progress_style.hpp"

namespace util {

struct ArchiveScanResult {
  archiveinfo::ArchiveReport report;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ArchiveScanResult inspectArchive(
    int descriptor, int progressDescriptor = -1,
    ProgressStyle progressStyle = ProgressStyle::Ascii);
std::string archiveLibraryVersion();

}  // namespace util
