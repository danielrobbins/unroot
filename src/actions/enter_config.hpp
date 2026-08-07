#pragma once
#include "config_base.hpp"
#include "option_parser.hpp"
#include "../emulation.hpp"
#include "linuxns.hpp"
#include "util/idmap.hpp"
#include <vector>
#include <string>
#include <memory>

namespace actions {

// Forward declaration
class EnterAction;

class EnterConfig : public ActionConfig {
public:
    using ActionClass = EnterAction; // Specify the action class for generic runner
    
    // Core rootfs properties (mutable for lazy initialization)
    mutable std::string root;
    mutable bool single = false;
    mutable bool native = false;
    mutable emulation::Policy emulationPolicy = emulation::Policy::Auto;
    mutable bool emulationSpecified = false;
    mutable std::string qemu;
    mutable std::string qemuCpu;
    
    // Architecture information (populated during analysis, mutable for lazy initialization)
    mutable std::string hostArch;
    mutable std::string targetArch;
    mutable std::string targetExecutable;
    mutable bool isCrossArch = false;
    
    // Namespace configuration (mutable for lazy initialization)
    mutable std::vector<BindMap> maps;
    mutable std::string cwdInRoot;
    mutable NsEnvVars envVars;
    mutable bool noDefaultEnv = false;
    
    // Enter-specific configuration (mutable for lazy initialization)
    mutable std::vector<std::string> shell;
    mutable std::vector<std::string> persistEnvNames;
    
    // Default constructor for two-stage initialization
    EnterConfig() = default;
    
    // ActionConfig interface - simplified
    std::string getActionName() const override { return "enter"; }
    void validate() const override;
    
    // Additional setup methods
    void validateRootfs() const;
    void validateNamespace() const;
    void analyzeArchitecture();
    void resolveEnvironment();
    void resolveRelativePaths();
    
    // Static handle method for ActionRegistry
    static int handle(const ToBeParsedArgs& args);
    
protected:
    // Non-const configure pattern implementation
    void configure_parser() override;
    void configureExecutionOptions();
    
    // Override postParse to handle trailing args (commands after --)
    void postParse(const OptionParser::ParseResult& result) override;

private:
    void parsePersistEnv(const std::string& csv) const;
    void addEnvironmentVariable(const std::string& kv) const;
    void addBindMap(const std::string& spec, bool readonly) const;
};

class SingleConfig : public EnterConfig {
public:
    using ActionClass = EnterAction;

    SingleConfig() { single = true; }
    std::string getActionName() const override { return "single"; }
    static int handle(const ToBeParsedArgs& args);

protected:
    void configure_parser() override;
};

} // namespace actions
