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

class EnterAction;

class EnterConfig : public ActionConfig {
public:
    using ActionClass = EnterAction;

    mutable std::string root;
    mutable bool singleId = false;
    mutable bool native = false;
    mutable emulation::Policy emulationPolicy = emulation::Policy::Auto;
    mutable bool emulationSpecified = false;
    mutable std::string qemu;
    mutable std::string qemuCpu;

    mutable std::string hostArch;
    mutable std::string targetArch;
    mutable std::string targetExecutable;
    mutable bool isCrossArch = false;

    mutable std::vector<BindMap> maps;
    mutable std::string cwdInRoot;
    mutable NsEnvVars envVars;
    mutable bool noDefaultEnv = false;

    mutable std::vector<std::string> shell;
    mutable std::vector<std::string> persistEnvNames;

    EnterConfig() = default;

    std::string getActionName() const override { return "enter"; }
    void validate() const override;

    void validateRootfs() const;
    void validateNamespace() const;
    void analyzeArchitecture();
    void resolveEnvironment();
    void resolveRelativePaths();

    static int handle(const ToBeParsedArgs& args);

protected:
    void configure_parser() override;
    void configureExecutionOptions();
    void postParse(const OptionParser::ParseResult& result) override;

private:
    void parsePersistEnv(const std::string& csv) const;
    void addEnvironmentVariable(const std::string& kv) const;
    void addBindMap(const std::string& spec, bool readonly) const;
};

} // namespace actions
