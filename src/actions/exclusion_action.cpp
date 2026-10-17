#include "rootfs_items_config.hpp"

#include <filesystem>
#include <iostream>

#include "app_exception.hpp"
#include "exclusions.hpp"
#include "nlohmann/json.hpp"
#include "parsed_args.hpp"
#include "unified_action_registry.hpp"
#include "util/archive_lock.hpp"
#include "util/error_map.hpp"

namespace actions {
class ExclusionConfig;

class ExclusionAction {
 public:
  static int perform(const ExclusionConfig& config);
};

class ExclusionConfig : public RootfsItemsConfig {
 public:
  using ActionClass = ExclusionAction;
  std::string getActionName() const override { return "exclude"; }
  static int handle(const ToBeParsedArgs& args) {
    return ActionConfig::run<ExclusionConfig>(args);
  }
};

int ExclusionAction::perform(const ExclusionConfig& config) {
  const auto fail = [](const std::string& error) {
    throw AppException(util::make_error(util::LibErr::Invalid, 0, error), "exclude");
  };
  std::error_code pathError;
  const auto path = std::filesystem::canonical(config.root, pathError);
  if (pathError) fail("unable to resolve ROOT: " + pathError.message());
  auto lock = util::acquireArchiveLock(path, false);
  if (!lock) fail(lock.error);
  if (!lock.matchesRoot()) fail("ROOT changed during exclusion operation");
  const auto root = lock.pinnedRoot().string();
  std::string error;
  if (config.operation == "list") {
    std::vector<std::string> patterns;
    if (!exclusions::list(root, patterns, error)) fail(error);
    if (config.json)
      std::cout << nlohmann::json{{"version", "unroot.exclusions/v1"},
                                {"patterns", patterns}}.dump(2) << '\n';
    else if (patterns.empty())
      std::cout << "No exclusions registered.\n";
    else
      for (const auto& pattern : patterns) std::cout << pattern << '\n';
  } else if (!exclusions::update(root, config.operation, config.items, error)) {
    fail(error);
  }
  if (!lock.matchesRoot()) fail("ROOT changed during exclusion operation");
  return 0;
}
}  // namespace actions

namespace {
const bool registered = []() {
  actions::ActionRegistry::register_action(
      "exclude", actions::ExclusionConfig::handle,
      "Manage paths omitted from rootfs archives",
      "Use list, add, remove, or clear. Rules affect packing only; rootfs files "
      "are never removed or replaced.",
      "OPERATION ROOT [PATTERN...] [OPTIONS]",
      []() { return std::make_unique<actions::ExclusionConfig>(); });
  return true;
}();
}  // namespace
