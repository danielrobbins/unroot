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
    return action_name.has_value() &&
           (std::find(args.begin(), args.end(), "--help") != args.end() ||
            std::find(args.begin(), args.end(), "-h") != args.end());
}

std::string ToBeParsedArgs::to_string() const {
    std::ostringstream oss;
    oss << "ToBeParsedArgs{";
    
    if (action_name.has_value()) {
        oss << "action='" << *action_name << "'";
    } else {
        oss << "action=null";
    }
    
    oss << ", global_help=" << (is_global_help() ? "true" : "false");
    oss << ", action_help=" << (is_action_help() ? "true" : "false");
    
    if (!args.empty()) {
        oss << ", args=[";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << "'" << args[i] << "'";
        }
        oss << "]";
    }
    
    oss << "}";
    return oss.str();
}

} // namespace actions
