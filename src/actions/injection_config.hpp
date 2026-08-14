#pragma once

#include <string>
#include <vector>

#include "config_base.hpp"

namespace actions {

class InjectionAction;

class InjectionConfig : public ActionConfig {
 public:
  using ActionClass = InjectionAction;

  std::string operation;
  std::string root;
  std::vector<std::string> items;
  bool json = false;

  std::string getActionName() const override { return "inject"; }
  void validate() const override;
  static int handle(const ToBeParsedArgs& args);

 protected:
  void configure_parser() override;
  void postParse(const OptionParser::ParseResult& result) override;
};

}  // namespace actions
