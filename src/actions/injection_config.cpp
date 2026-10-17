#include "injection_config.hpp"

#include "injection_action.hpp"
#include "parsed_args.hpp"
#include "unified_action_registry.hpp"

namespace actions {
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
