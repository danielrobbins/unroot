#pragma once
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

namespace actions {

/**
 * Structural preprocessing of command-line arguments.
 * 
 * Eliminates the need to pass raw argc/argv around while keeping
 * parsing responsibility in the Config classes where it belongs.
 * 
 * Responsibilities:
 * - Extract action name from argv[1]
 * - Collect remaining args as vector
 * - Detect global vs action-specific help
 * 
 * Does NOT handle:
 * - Option parsing (belongs in Config classes)
 * - Business logic validation
 * - Action-specific semantics
 */
struct ToBeParsedArgs {
    std::optional<std::string> action_name;  // None for "unroot --help"
    std::vector<std::string> args;           // Raw args for Config to parse
    
    // Factory method
    static ToBeParsedArgs from_argv(int argc, char** argv);
    
    // Help detection utilities
    bool is_global_help() const;
    bool is_action_help() const;
    
    // Debugging/logging
    std::string to_string() const;
};

} // namespace actions
