#include "injection_config.hpp"

#include <filesystem>

#include "app_exception.hpp"
#include "injection_action.hpp"
#include "parsed_args.hpp"
#include "unified_action_registry.hpp"
#include "util/error_map.hpp"

namespace actions {

void InjectionConfig::configure_parser() {
  ActionConfig::configure_parser();
  parser_
      .allow_trailing_args()
      .add_flag_meta({"--json"}, "Emit list output as JSON",
                     [this]() { json = true; })
      .add_flag_meta({"--help", "-h"}, "Display help for this action", []() {})
      .add_positional_meta(
          "OPERATION", "list, add, remove, or clear",
          [this](const std::string& value) { operation = value; })
      .add_positional_meta("ROOT", "Managed root filesystem",
                           [this](const std::string& value) { root = value; });
}

void InjectionConfig::postParse(const OptionParser::ParseResult& result) {
  items = result.trailing_args;
}

void InjectionConfig::validate() const {
  if (operation != "list" && operation != "add" && operation != "remove" &&
      operation != "clear")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inject operation must be list, add, remove, or clear"),
        "usage");
  if (!std::filesystem::is_directory(root))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "root path is not a directory: " + root),
        "usage");
  if ((operation == "add" || operation == "remove") && items.empty())
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inject " + operation + " requires at least one item"),
        "usage");
  if ((operation == "list" || operation == "clear") && !items.empty())
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inject " + operation + " does not accept items"),
        "usage");
  if (json && operation != "list")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "--json applies only to inject list"),
        "usage");
}

int InjectionConfig::handle(const ToBeParsedArgs& args) {
  return ActionConfig::run<InjectionConfig>(args);
}

}  // namespace actions

namespace {
const bool inject_registered = []() {
  actions::ActionRegistry::register_action(
      "inject", actions::InjectionConfig::handle,
      "Inspect and manage content injected into a managed rootfs",
      "Use list, add, remove, or clear to manage durable files and links while "
      "preserving the portable rootfs originals.",
      "OPERATION ROOT [ITEM...] [OPTIONS]",
      []() { return std::make_unique<actions::InjectionConfig>(); });
  return true;
}();
}  // namespace
