// Test-only stub for EnterAction to satisfy template instantiations in EnterConfig
// This avoids pulling in heavy runtime dependencies (linuxns, meta, emulation, etc.)
#include "actions/enter_action.hpp"

namespace actions {

int EnterAction::execute(const EnterConfig&) {
    // No-op for doctest parsing scenarios
    return 0;
}

// Private helpers (provide trivial definitions to satisfy linker; not used in doctests)
std::unique_ptr<EmuPlan> EnterAction::setupEmulation(
    const EnterConfig&, const util::IdMapPlan&) { return {}; }
util::IdMapPlan EnterAction::resolveIdMap(const EnterConfig&) { return {}; }
int EnterAction::executeNamespace(const EnterConfig&,
                                  const std::unique_ptr<EmuPlan>&,
                                  const util::IdMapPlan&) { return 0; }

} // namespace actions
