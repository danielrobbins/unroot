#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace archiveinfo {

inline constexpr const char* Protocol = "unroot-archive-v1";
inline constexpr size_t ResponseLimit = 64 * 1024;

struct Finding {
  uint64_t count = 0;
  std::vector<std::string> examples;
};

struct ArchiveReport {
  std::string format;
  std::vector<std::string> filters;
  uint64_t members = 0;
  uint64_t regularBytes = 0;
  uint64_t maxUid = 0;
  uint64_t maxGid = 0;
  bool reservedMetadata = false;
  bool ociLayout = false;
  Finding unsafePaths;
  Finding acls;
  Finding xattrs;
  Finding capabilities;
  Finding selinuxLabels;
  Finding sparseFiles;
  Finding hardLinks;
  Finding symbolicLinks;
  Finding devices;
  Finding fifos;
  Finding sockets;

  nlohmann::json toJson() const;
};

struct ReportResult {
  ArchiveReport report;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

ReportResult parseRecord(const std::string& record);
std::string makeRecord(const ArchiveReport& report);

}  // namespace archiveinfo
