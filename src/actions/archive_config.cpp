#include "archive_config.hpp"

#include <filesystem>
#include <unistd.h>

#include "app_exception.hpp"
#include "archive_action.hpp"
#include "parsed_args.hpp"
#include "unified_action_registry.hpp"
#include "util/error_map.hpp"

namespace actions {

void PackConfig::configure_parser() {
  ActionConfig::configure_parser();
  parser_
      .add_flag_meta(
          {"--force"}, "Continue when GNU tar cannot preserve all metadata",
          [this]() { force = true; })
      .add_flag_meta({"--help", "-h"}, "Display help for this action", []() {})
      .add_positional_meta("ROOT", "Mapped root filesystem to archive",
                           [this](const std::string& value) { root = value; })
      .add_positional_meta(
          "ARCHIVE", "Destination tar archive",
          [this](const std::string& value) { archive = value; });
}

void PackConfig::validate() const {
  if (!std::filesystem::is_directory(root))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "root path is not a directory: " + root),
        "usage");
  if (archive == "-")
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "streaming archives are not yet supported"),
        "usage");
}

int PackConfig::handle(const ToBeParsedArgs& args) {
  return ActionConfig::run<PackConfig>(args);
}

void UnpackConfig::configure_parser() {
  ActionConfig::configure_parser();
  parser_
      .add_flag_meta(
          {"--native"},
          "Preserve host-native ownership instead of using subordinate IDs",
          [this]() { native = true; })
      .add_flag_meta(
          {"--force"}, "Continue when GNU tar cannot preserve all metadata",
          [this]() { force = true; })
      .add_flag_meta({"--help", "-h"}, "Display help for this action", []() {})
      .add_positional_meta(
          "ARCHIVE", "Source tar archive",
          [this](const std::string& value) { archive = value; })
      .add_positional_meta("ROOT", "New or empty root filesystem directory",
                           [this](const std::string& value) { root = value; });
}

void UnpackConfig::validate() const {
  if (!std::filesystem::is_regular_file(archive))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "archive is not a regular file: " + archive),
        "usage");
  if (std::filesystem::exists(root) && !std::filesystem::is_directory(root))
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "root path is not a directory: " + root),
        "usage");
  if (native && ::geteuid() != 0)
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "--native requires host root privileges; run with sudo"),
        "usage");
}

int UnpackConfig::handle(const ToBeParsedArgs& args) {
  return ActionConfig::run<UnpackConfig>(args);
}

}  // namespace actions

namespace {
const bool pack_registered = []() {
  actions::ActionRegistry::register_action(
      "pack", actions::PackConfig::handle,
      "Capture a mapped root filesystem as a tar archive",
      "Preserves numeric ownership and extended filesystem metadata while "
      "excluding .unroot.",
      "ROOT ARCHIVE [OPTIONS]",
      []() { return std::make_unique<actions::PackConfig>(); });
  return true;
}();

const bool unpack_registered = []() {
  actions::ActionRegistry::register_action(
      "unpack", actions::UnpackConfig::handle,
      "Create a mapped root filesystem from a tar archive",
      "Initializes durable ownership metadata and restores full filesystem "
      "metadata; rich subordinate-ID ownership is the default.",
      "ARCHIVE ROOT [OPTIONS]",
      []() { return std::make_unique<actions::UnpackConfig>(); });
  return true;
}();
}  // namespace
