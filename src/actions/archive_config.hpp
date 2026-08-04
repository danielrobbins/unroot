#pragma once

#include <string>

#include "config_base.hpp"
#include "util/idmap.hpp"

namespace actions {

class ArchiveAction;

class PackConfig : public ActionConfig {
 public:
  using ActionClass = ArchiveAction;

  std::string root;
  std::string archive;

  std::string getActionName() const override { return "pack"; }
  void validate() const override;
  static int handle(const ToBeParsedArgs& args);

 protected:
  void configure_parser() override;
};

class UnpackConfig : public ActionConfig {
 public:
  using ActionClass = ArchiveAction;

  std::string archive;
  std::string root;
  bool native = false;

  std::string getActionName() const override { return "unpack"; }
  void validate() const override;
  static int handle(const ToBeParsedArgs& args);

 protected:
  void configure_parser() override;

};

}  // namespace actions
