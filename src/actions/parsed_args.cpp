#include "parsed_args.hpp"
#include <sstream>

namespace actions {

ToBeParsedArgs ToBeParsedArgs::from_argv(int argc, char** argv) {
    ToBeParsedArgs result;
    
    if (argc < 2) {
        return result; // No action, no args
    }
    
    // Check for global help first
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        // Global help: "unroot --help"
        return result; // action_name is nullopt
    }
    
    // We have an action
    result.action_name = argv[1];
    
    // Collect remaining args (including potential --help for action)
    for (int i = 2; i < argc; ++i) {
        result.args.push_back(argv[i]);
    }
    
    return result;
}

bool ToBeParsedArgs::is_global_help() const {
    return !action_name.has_value();
}

bool ToBeParsedArgs::is_action_help() const {
    if (!action_name.has_value()) return false;
    const auto end = std::find(args.begin(), args.end(), "--");
    return std::find(args.begin(), end, "--help") != end ||
           std::find(args.begin(), end, "-h") != end;
}

} // namespace actions
