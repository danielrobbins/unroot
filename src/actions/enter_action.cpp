#include "enter_action.hpp"
#include "../linuxns.hpp"
#include "../include/util/app_exit.hpp"
#include "../emulation.hpp"
#include "../meta.hpp"
#include "../include/util/log_host.hpp"
#include "../include/util/error_map.hpp"
#include "../app_exception.hpp"
#include <iostream>
#include <memory>
#include <unistd.h>

namespace actions {

int EnterAction::execute(const EnterConfig& config) {
    try {
        auto idmap = resolveIdMap(config);

        // Step 1: Setup emulation - critical for cross-architecture execution
        auto emuPlan = setupEmulation(config, idmap);

        // Step 2: Execute namespace with the persisted ID map
        return executeNamespace(config, emuPlan, idmap);
        
    } catch (const AppException& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return app::toInt(app::Exit::Usage);
    } catch (const std::exception& e) {
        std::cerr << "Enter failed: " << e.what() << std::endl;
        return app::toInt(app::Exit::Unknown);
    }
}

util::IdMapPlan EnterAction::resolveIdMap(const EnterConfig& config) {
    auto fail = [](std::string message) -> void {
        if (message.rfind("error: ", 0) == 0) message.erase(0, 7);
        throw AppException(
            util::make_error(util::LibErr::Invalid, 0, std::move(message)),
            "idmap");
    };
    if (config.singleId) {
        if (!config.root.empty()) {
            auto stored = meta::readIdMap(config.root);
            if (!stored.error.empty()) fail(stored.error);
            if (stored.found)
                fail("--single conflicts with the ownership mode recorded for ROOT");
        }
        return util::makeSingleIdMap(::getuid(), ::getgid());
    }

    auto stored = meta::readIdMap(config.root);
    if (!stored.error.empty()) fail(stored.error);
    if (!stored.found) {
        if (!config.native)
            fail("rootfs has no Unroot ownership metadata; create it with "
                 "'unroot unpack', or use 'sudo unroot enter --native' for "
                 "an unmanaged host-owned rootfs");
        auto native = meta::selectIdMap(util::IdMapMode::Native, 0);
        if (!native) fail(native.error);
        return std::move(native.plan);
    }
    if (config.native && stored.plan.mode != util::IdMapMode::Native)
        fail("--native conflicts with the ownership mode recorded for ROOT");

    auto result = meta::resolveIdMap(
        config.root, stored.plan.mode, util::subordinateIdCount(stored.plan),
        false);
    if (!result) fail(result.error);
    if (result.plan.mode == util::IdMapMode::Rich)
        util::hostLogStep("idmap:source", true, result.plan.source);
    return std::move(result.plan);
}

// Helper method: Setup emulation following working reference implementation
std::unique_ptr<EmuPlan> EnterAction::setupEmulation(
    const EnterConfig& config, const util::IdMapPlan& idmap) {
    // First analyze architecture to populate host/target arch fields
    const_cast<EnterConfig&>(config).analyzeArchitecture();
    
    // Log architecture information
    util::hostLogStep("arch:host", !config.hostArch.empty(), config.hostArch);
    util::hostLogStep("arch:target", !config.targetArch.empty(), config.targetArch);
    util::hostLogStep("arch:cross", config.isCrossArch, config.isCrossArch ? "true" : "false");
    util::hostLogStep("emu:policy", true,
                      emulation::policyName(config.emulationPolicy));
    if (!config.qemuCpu.empty())
        util::hostLogStep("emu:cpu", true, config.qemuCpu);

    std::string command = config.shell.empty() ? "/bin/sh" : config.shell[0];
    for (size_t i = 1; i < config.shell.size(); ++i) {
        command.push_back(' ');
        command += config.shell[i];
    }
    util::hostLogStep("exec:first", true, command);

    emulation::Settings settings;
    settings.hostVisible = config.hostVisible;
    settings.native = idmap.mode == util::IdMapMode::Native;
    settings.root = config.root;
    settings.executable = config.targetExecutable;
    settings.targetArch = config.targetArch;
    settings.hostArch = config.hostArch;
    settings.policy = config.emulationPolicy;
    settings.qemu = config.qemu;
    settings.cpu = config.qemuCpu;

    emulation::Manager manager(std::move(settings));
    EmuPlan plan = manager.prepare();
    if (plan.source.empty()) return {};
    util::hostLogStep("qemu:mode", true,
                      std::string("private binfmt, emulator=") + plan.source);
    return std::make_unique<EmuPlan>(std::move(plan));
}

// Helper method: Execute namespace with proper error handling
int EnterAction::executeNamespace(const EnterConfig& config,
                                  const std::unique_ptr<EmuPlan>& emuPlan,
                                  const util::IdMapPlan& idmap) {
    NsResult r{};
    try {
        EmuPlan* emuPtr = (emuPlan && !emuPlan->source.empty()) ? emuPlan.get() : nullptr;

        r = enterNamespace(config.root, config.shell, idmap, emuPtr,
                           config.maps.empty() ? nullptr : &config.maps,
                           config.envVars.empty() ? nullptr : &config.envVars,
                           config.cwdInRoot.empty() ? nullptr : &config.cwdInRoot);
    } catch (const std::system_error& se) {
        util::hostLogStep("enter:sys_error", false, se.what());
        std::cerr << "enter system_error: category=" << se.code().category().name() 
                  << " value=" << se.code().value() << " what=" << se.what() << "\n";
        return app::toInt(app::Exit::Unknown);
    } catch (const std::exception& ex) {
        util::hostLogStep("enter:exception", false, ex.what());
        std::cerr << "enter exception: " << ex.what() << "\n";
        return app::toInt(app::Exit::Unknown);
    }

    // Propagate raw child exit (sentinel -1) before classification
    if (r.code == -1) {
        int ec = 0;
        try {
            ec = std::stoi(r.msg);
        } catch (...) {
            ec = 1;
        }
        return ec;
    }

    if (r.code != 0) {
        std::cerr << "enter failed: " << r.msg << " (code=" << r.code << ")\n";
    }

    return r.code;
}

} // namespace actions
