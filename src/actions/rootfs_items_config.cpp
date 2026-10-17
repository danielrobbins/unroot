#include "rootfs_items_config.hpp"

#include <filesystem>

#include "app_exception.hpp"
#include "parsed_args.hpp"
#include "util/error_map.hpp"

namespace actions {

void RootfsItemsConfig::configure_parser() {
  ActionConfig::configure_parser();
  parser_
      .allow_trailing_args()
      .add_flag_meta({"--json"}, "Emit list output as JSON",
                     [this]() { json = true; })
      .add_flag_meta({"--help", "-h"}, "Display help for this action", []() {})
      .add_positional_meta(
          "OPERATION", "list, add, remove, or clear",
          [this](const std::string& value) { operation = value; })
      .add_positional_meta("ROOT", "Root filesystem",
                           [this](const std::string& value) { root = value; });
}

void RootfsItemsConfig::postParse(const OptionParser::ParseResult& result) {
  items = result.trailing_args;
}

void RootfsItemsConfig::validate() const {
  const auto action = getActionName();
  if (operation != "list" && operation != "add" && operation != "remove" &&
      operation != "clear")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         action + " operation must be list, add, remove, or clear"),
        "usage");
  if (!std::filesystem::is_directory(root))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "root path is not a directory: " + root),
        "usage");
  if ((operation == "add" || operation == "remove") && items.empty())
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         action + " " + operation + " requires at least one item"),
        "usage");
  if ((operation == "list" || operation == "clear") && !items.empty())
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         action + " " + operation + " does not accept items"),
        "usage");
  if (json && operation != "list")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "--json applies only to " + action + " list"),
        "usage");
}

}  // namespace actions
