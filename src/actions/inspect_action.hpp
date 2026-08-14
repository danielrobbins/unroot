#pragma once

namespace actions {

class InspectConfig;

class InspectAction {
 public:
  static int perform(const InspectConfig& config);
};

}  // namespace actions
