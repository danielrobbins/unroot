
#include "enter_config.hpp"
#include "enter_action.hpp"
#include "unified_action_registry.hpp"
#include "parsed_args.hpp"
#include "option_parser.hpp"
#include "app_exception.hpp"
#include "util/error_map.hpp"
#include "util/exception_handler.hpp"
#include "../linuxns.hpp"
#include "../arch.hpp"
#include "../shebang.hpp"
#include "util/path.hpp"
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <filesystem>

namespace actions {

namespace {
constexpr const char* defaultPath =
    "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
}

// Virtual configure pattern implementation for EnterConfig
void EnterConfig::configure_parser() {
    ActionConfig::configure_parser();
    configureExecutionOptions();
    parser_
        .add_flag_meta({"--native"},
            "Use host ownership and host root privileges for an unmanaged rootfs",
            [this]() { native = true; })
        .add_option_meta({"--emulation"}, "<mode>",
        "Set foreign execution policy: auto or never",
        [this](const std::string& value) {
            emulationSpecified = true;
            if (value == "auto") emulationPolicy = emulation::Policy::Auto;
            else if (value == "never") emulationPolicy = emulation::Policy::Never;
            else throw AppException(util::make_error(
                util::LibErr::Invalid, 0,
                "--emulation expects 'auto' or 'never'"), "usage");
        })
        .add_option_meta({"--qemu"}, "<path>",
        "Use this static QEMU user-mode emulator",
        [this](const std::string& value) { qemu = value; })
        .add_option_meta({"--qemu-cpu"}, "<spec>",
        "Set the QEMU CPU model and feature specification",
        [this](const std::string& value) { qemuCpu = value; })
        .add_multi_option_meta({"--map"}, "<path>", "Bind an absolute host path at the same path inside the new root (repeatable)",
        [this](const std::string& v) {
            if (v.empty() || v[0] != '/') {
                throw AppException(util::make_error(util::LibErr::Invalid, 0,
                    "--map expects absolute path"), "usage");
            }
            BindMap bm; bm.src = v; bm.dst = v; bm.readonly = false;
            maps.push_back(std::move(bm));
        })
        .add_multi_option_meta({"--map-ro"}, "<src:dst>",
            "Bind an absolute host path at dst read-only (repeatable)",
            [this](const std::string& v) { addBindMap(v, true); })
        .add_positional_meta("ROOT", "Root filesystem path",
            [this](const std::string& path) { root = path; });
}

void EnterConfig::configureExecutionOptions() {
    parser_.allow_trailing_args();
    parser_
        .add_option_meta({"--cwd"}, "<dir>",
            "Set the working directory for the target command",
            [this](const std::string& v) { cwdInRoot = v; })
        .add_multi_option_meta({"--env"}, "<var>",
            "Set environment variable (KEY=VALUE) (repeatable)",
            [this](const std::string& v) { addEnvironmentVariable(v); })
        .add_option_meta({"--persist-env"}, "<names>",
            "Comma-separated list of environment variables to preserve",
            [this](const std::string& v) { parsePersistEnv(v); })
        .add_flag_meta({"--no-default-env"},
            "Do not add Unroot's default PATH to the target environment",
            [this]() { noDefaultEnv = true; })
        .add_flag_meta({"--help", "-h"}, "Display help for this action", [](){});
}

void SingleConfig::configure_parser() {
    ActionConfig::configure_parser();
    configureExecutionOptions();
}

void EnterConfig::postParse(const OptionParser::ParseResult& result) {
    // Handle trailing arguments (command to execute after --)
    for (const auto& c : result.trailing_args) {
        shell.push_back(c);
    }
    
    // Call any remaining post-processing
    resolveEnvironment();
}

void EnterConfig::addBindMap(const std::string& spec, bool readonly) const {
    auto pos = spec.find(':');
    if (pos == std::string::npos) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            std::string("invalid map spec (expected src:dst): ") + spec), "usage");
    }
    
    std::string src = spec.substr(0, pos);
    std::string dst = spec.substr(pos + 1);
    
    if (src.empty() || src[0] != '/' || dst.empty() || dst[0] != '/') {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            std::string("map paths must be absolute: ") + spec), "usage");
    }
    
    BindMap bm;
    bm.src = src;
    bm.dst = dst;
    bm.readonly = readonly;
    maps.push_back(std::move(bm));
}

void EnterConfig::parsePersistEnv(const std::string& csv) const {
    size_t start = 0;
    while (start <= csv.size()) {
        size_t c = csv.find(',', start);
        std::string name = csv.substr(start, (c == std::string::npos ? csv.size() : c) - start);
        
        // Trim whitespace
        auto l = name.find_first_not_of(" \t");
        auto r = name.find_last_not_of(" \t");
        if (l != std::string::npos && r != std::string::npos) {
            name = name.substr(l, r - l + 1);
        } else if (l == std::string::npos) {
            name.clear();
        }
        
        if (!name.empty()) {
            persistEnvNames.push_back(name);
        }
        
        if (c == std::string::npos) {
            break;
        }
        start = c + 1;
    }
}

void EnterConfig::addEnvironmentVariable(const std::string& kv) const {
    auto eq = kv.find('=');
    if (eq == std::string::npos || eq == 0) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            "--env requires KEY=VALUE"), "usage");
    }
    
    std::string k = kv.substr(0, eq);
    std::string v = kv.substr(eq + 1);
    envVars.emplace_back(k, v);
}

void EnterConfig::validate() const {
    // Use rootfs validation
    validateRootfs();
    
    // Use namespace validation  
    validateNamespace();
    
    // Enter-specific validation can go here
}

void EnterConfig::validateRootfs() const {
    if (single) {
        if (!root.empty())
            throw AppException(util::make_error(
                util::LibErr::Invalid, 0, "single does not accept a rootfs"),
                "usage");
        return;
    }
    if (root.empty()) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            "enter requires ROOT"), "usage");
    }

    if (native && ::geteuid() != 0)
        throw AppException(util::make_error(
            util::LibErr::Invalid, 0,
            "--native requires host root privileges; run with sudo"), "usage");

    if (emulationPolicy == emulation::Policy::Never &&
        (!qemu.empty() || !qemuCpu.empty())) {
        throw AppException(util::make_error(
            util::LibErr::Invalid, 0,
            "--emulation=never cannot be combined with --qemu or --qemu-cpu"),
            "usage");
    }
    
    if (!root.empty() && !std::filesystem::exists(root)) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            std::string("root path does not exist: ") + root), "usage");
    }
}

void EnterConfig::validateNamespace() const {
    // Validate bind maps
    for (const auto& map : maps) {
        if (map.src.empty() || map.dst.empty()) {
            throw std::invalid_argument("Invalid bind map: source and destination cannot be empty");
        }
    }
    
}

void EnterConfig::analyzeArchitecture() {
    hostArch = detectHostArch();
    
    if (!root.empty()) {
        targetExecutable = shell.empty() ? "/bin/sh" : shell[0];
        if (!targetExecutable.empty() && targetExecutable[0] != '/') {
            std::string path;
            if (targetExecutable.find('/') == std::string::npos) {
                auto item = std::find_if(
                    envVars.begin(), envVars.end(),
                    [](const auto& value) { return value.first == "PATH"; });
                if (item == envVars.end() || item->second.empty()) {
                    throw AppException(
                        util::make_error(
                            util::LibErr::Invalid, 0,
                            "bare commands require PATH via --env or --persist-env"),
                        "usage");
                }
                path = item->second;
            }
            targetExecutable = util::resolveExecInRoot(
                root, cwdInRoot, targetExecutable, path);
        }
        if (!targetExecutable.empty()) {
            targetExecutable = resolveExecutableImage(root, targetExecutable);
            ElfInfo target = readElfInfoAtRoot(root, targetExecutable);
            if (target.valid && target.archName.empty()) {
                throw AppException(
                    util::make_error(
                        util::LibErr::Invalid, 0,
                        "unsupported ELF target: machine=" +
                            std::to_string(target.e_machine) + ", " +
                            elfClassToString(target.ei_class) + ", " +
                            elfDataToString(target.ei_data)),
                    "enter");
            }
            targetArch = target.archName;
        }
    } else {
        targetArch = hostArch;
        targetExecutable.clear();
    }
    
    isCrossArch = (!targetArch.empty() && !hostArch.empty() && targetArch != hostArch);
}

void EnterConfig::resolveEnvironment() {
    auto hasKey = [&](const std::string& key) {
        return std::any_of(envVars.begin(), envVars.end(),
            [&](const auto& value) { return value.first == key; });
    };

    if (!persistEnvNames.empty()) {
        for (const auto& name : persistEnvNames) {
            if (name.empty() || hasKey(name)) continue;
            
            const char* v = ::getenv(name.c_str());
            if (v && *v) {
                envVars.emplace_back(name, std::string(v));
            }
        }
    }

    if (!noDefaultEnv && !hasKey("PATH"))
        envVars.emplace_back("PATH", defaultPath);
}

int EnterConfig::handle(const ToBeParsedArgs& args) {
    return ActionConfig::run<EnterConfig>(args);
}

int SingleConfig::handle(const ToBeParsedArgs& args) {
    return ActionConfig::run<SingleConfig>(args);
}

} // namespace actions

namespace {
    static bool enter_registered = []() {
        actions::ActionRegistry::register_action(
            "enter",
            actions::EnterConfig::handle,
            "Enter an initialized root filesystem environment",
            "Uses the ownership mode recorded in ROOT/.unroot/meta.json; "
            "--native explicitly enters an unmanaged host-owned rootfs.",
            "ROOT [OPTIONS] [-- COMMAND [ARGUMENTS...]]",
            []() { return std::make_unique<actions::EnterConfig>(); }
        );
        return true;
    }();

    static bool single_registered = []() {
        actions::ActionRegistry::register_action(
            "single",
            actions::SingleConfig::handle,
            "Run with a single-identity user namespace",
            "Keeps the host filesystem visible while mapping the invoking "
            "user to namespace root.",
            "[OPTIONS] [-- COMMAND [ARGUMENTS...]]",
            []() { return std::make_unique<actions::SingleConfig>(); }
        );
        return true;
    }();
}
