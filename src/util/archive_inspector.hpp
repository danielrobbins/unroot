#pragma once

#include <string>

#include "archive_report.hpp"

namespace util {

struct ArchiveScanResult {
  archiveinfo::ArchiveReport report;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ArchiveScanResult inspectArchive(int descriptor);
std::string archiveLibraryVersion();

}  // namespace util
