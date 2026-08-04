#pragma once
#include "enter_config.hpp"
#include "../linuxns.hpp"
#include "util/idmap.hpp"
#include <memory>

namespace actions {

class EnterAction {
public:
    // This will be the dedicated entry point for the enter action.
    static int execute(const EnterConfig& config);
    
    // Generic runner interface - calls execute
    static int perform(const EnterConfig& config) { return execute(config); }
    
private:
    // Helper methods for the enter execution flow
    static std::unique_ptr<EmuPlan> setupEmulation(
        const EnterConfig& config, const util::IdMapPlan& idmap);
    static util::IdMapPlan resolveIdMap(const EnterConfig& config);
    static int executeNamespace(const EnterConfig& config,
                                const std::unique_ptr<EmuPlan>& emuPlan,
                                const util::IdMapPlan& idmap);
};

}
