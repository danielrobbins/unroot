#pragma once

#include <string>

#include "config_base.hpp"

namespace actions {

class InspectAction;

class InspectConfig : public ActionConfig {
 public:
  using ActionClass = InspectAction;

  std::string subject;
  std::string archive;
  bool json = false;

  std::string getActionName() const override { return "inspect"; }
  void validate() const override;
  static int handle(const ToBeParsedArgs& args);

 protected:
  void configure_parser() override;
};

}  // namespace actions
