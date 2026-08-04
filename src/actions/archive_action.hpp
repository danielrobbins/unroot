#pragma once

namespace actions {

class PackConfig;
class UnpackConfig;

class ArchiveAction {
 public:
  static int perform(const PackConfig& config);
  static int perform(const UnpackConfig& config);
};

}  // namespace actions
