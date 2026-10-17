#pragma once

#include <string>
#include <vector>
#include "config_base.hpp"

namespace actions {
class RootfsItemsConfig : public ActionConfig {
 public:
  std::string operation;
  std::string root;
  std::vector<std::string> items;
  bool json = false;
  void validate() const override;

 protected:
  void configure_parser() override;
  void postParse(const OptionParser::ParseResult& result) override;
};
}  // namespace actions
