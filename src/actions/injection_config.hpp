#pragma once

#include <string>
#include <vector>

#include "rootfs_items_config.hpp"

namespace actions {

class InjectionAction;

class InjectionConfig : public RootfsItemsConfig {
 public:
  using ActionClass = InjectionAction;

  std::string getActionName() const override { return "inject"; }
  static int handle(const ToBeParsedArgs& args);
};

}  // namespace actions
