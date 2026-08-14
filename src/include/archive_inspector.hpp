#pragma once

#include <string>

#include "archive_input.hpp"
#include "archive_report.hpp"

namespace archiveio {

struct InspectionResult {
  archiveinfo::ArchiveReport report;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

class Inspector {
 public:
  InspectionResult inspect(const Input& input) const;
};

}  // namespace archiveio
