// Test-only stub for enterNamespace, which injections.cpp calls to run the mapped helper.
// The doctests never enter a namespace, so this avoids linking linuxns.cpp.
#include "linuxns.hpp"

NsResult enterNamespace(const std::string&,
                        const std::vector<std::string>&,
                        const util::IdMapPlan&,
                        const EmuPlan*,
                        const std::vector<BindMap>*,
                        const NsEnvVars*,
                        const std::string*) {
    return {1, "enterNamespace is not available in doctests"};
}
