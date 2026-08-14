#include "inspect_config.hpp"

#include <filesystem>

#include "app_exception.hpp"
#include "inspect_action.hpp"
#include "parsed_args.hpp"
#include "unified_action_registry.hpp"
#include "util/error_map.hpp"

namespace actions {

void InspectConfig::configure_parser() {
  ActionConfig::configure_parser();
  parser_
      .add_flag_meta({"--json"}, "Emit the complete machine-readable report",
                     [this]() { json = true; })
      .add_flag_meta({"--help", "-h"}, "Display help for this action", []() {})
      .add_positional_meta("SUBJECT", "host or archive",
                           [this](const std::string& value) { subject = value; })
      .add_positional_meta(
          "ARCHIVE", "Archive to inspect",
          [this](const std::string& value) { archive = value; }, false);
}

void InspectConfig::validate() const {
  if (subject != "host" && subject != "archive")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inspect subject must be 'host' or 'archive'"),
        "usage");
  if (subject == "host" && !archive.empty())
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inspect host does not accept an archive"),
        "usage");
  if (subject == "archive" &&
      (archive.empty() || !std::filesystem::is_regular_file(archive)))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "inspect archive requires a regular archive file"),
        "usage");
}

int InspectConfig::handle(const ToBeParsedArgs& args) {
  return ActionConfig::run<InspectConfig>(args);
}

namespace {
const bool inspectRegistered = []() {
  ActionRegistry::register_action(
      "inspect", InspectConfig::handle,
      "Inspect the host or an archive before changing a rootfs",
      "Reports namespace and helper capabilities or structured archive "
      "contents without mutating the host, archive, or rootfs.",
      "SUBJECT [ARCHIVE] [OPTIONS]",
      []() { return std::make_unique<InspectConfig>(); });
  return true;
}();
}  // namespace

}  // namespace actions
