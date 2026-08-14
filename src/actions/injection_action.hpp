#pragma once

namespace actions {

class InjectionConfig;

class InjectionAction {
 public:
  static int perform(const InjectionConfig& config);
};

}  // namespace actions
