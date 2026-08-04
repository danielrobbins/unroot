#pragma once
#include <string>
#include <vector>
#include <memory>
#include "option_parser.hpp"
#include "util/exception_handler.hpp" // For the generic run method

namespace actions {

struct ToBeParsedArgs;
class Action; // Forward declaration

// Represents a command-line option for an action
struct Option {
    std::string name;           // e.g., "--cwd"
    std::string arg_name;       // e.g., "<dir>" or empty for flags
    std::vector<std::string> description_lines;  // Help text lines for this option
    bool required = false;      // Whether this option is required
    bool repeatable = false;    // Whether option can be used multiple times
    
    Option(const std::string& name, const std::string& arg_name, const std::string& description, 
           bool required = false, bool repeatable = false)
        : name(name), arg_name(arg_name), required(required), repeatable(repeatable) {
        description_lines.push_back(description);
    }
    
    Option(const std::string& name, const std::string& arg_name, const std::vector<std::string>& description_lines, 
           bool required = false, bool repeatable = false)
        : name(name), arg_name(arg_name), description_lines(description_lines), required(required), repeatable(repeatable) {}
};

// Base configuration class for all actions
class ActionConfig {
public:
    using ActionClass = Action; // Default, can be overridden

    virtual ~ActionConfig() = default;

    // The only remaining required metadata method
    virtual std::string getActionName() const = 0;

    // --- Core Lifecycle Methods ---
    virtual void validate() const;
    void parse(const ToBeParsedArgs& args); // Now non-virtual
    std::vector<Option> options() const;

protected:
    // Hook for subclasses that need access to full ParseResult (e.g., trailing_args)
    virtual void postParse(const OptionParser::ParseResult& result);

    // --- Generic Runner ---
    template<typename ConfigType>
    static int run(const ToBeParsedArgs& args) {
        return util::handle_exceptions([&]() {
            ConfigType config;
            config.parse(args);
            config.validate();
            return ConfigType::ActionClass::perform(config);
        }, util::ErrorContext::CLI);
    }

protected:
    // --- Parser Configuration ---
    const OptionParser& getParser() const;
    virtual void configure_parser(); // Now non-const

    OptionParser parser_;

private:
    mutable bool parser_configured_ = false;
};

// Base class for all filesystem operations
class FilesystemConfig : public ActionConfig {
public:
    std::string root;
    void validate() const override;

protected:
    void configure_parser() override;
};

} // namespace actions
