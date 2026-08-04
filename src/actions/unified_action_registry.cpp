#include "unified_action_registry.hpp"
#include "config_base.hpp"
#include "parsed_args.hpp"
#include "../program_context.hpp"
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <iomanip>

namespace actions {

// Action implementation
Action::Action(const std::string& name,
               std::function<int(const ToBeParsedArgs&)> handler,
               const std::string& brief,
               const std::string& description,
               const std::string& usage_pattern,
               ConfigFactory config_factory)
    : name_(name), brief_(brief), description_(description), usage_pattern_(usage_pattern),
      handler_(handler), config_factory_(config_factory) {
}

std::string Action::help() const {
    if (is_container()) {
        return generate_container_help();
    } else {
        return generate_action_help();
    }
}

bool Action::is_container() const {
    return !sub_actions().empty();
}

std::vector<std::string> Action::sub_actions() const {
    return ActionRegistry::find_sub_actions(name_);
}

int Action::execute(const ToBeParsedArgs& args) const {
    if (is_container()) {
        // For container actions, show help or error
        if (args.is_action_help()) {
            std::cout << help();
            return 0;
        } else {
            std::cerr << "Error: " << name_ << " requires a sub-action\n";
            std::cerr << "Use '" << program::Context::program_name() << " " << name_ << " --help' to see available operations.\n";
            return 1;
        }
    } else {
        // For leaf actions, call handler directly with ToBeParsedArgs
        return handler_(args);
    }
}

std::string Action::generate_container_help() const {
    std::ostringstream oss;
    
    oss << "Usage: " << program::Context::program_name() << " " << name_ << " SUBCOMMAND [OPTIONS]\n\n";
    oss << brief_ << "\n\n";
    
    if (!description_.empty() && description_ != brief_) {
        oss << description_ << "\n\n";
    }
    
    oss << "Available subcommands:\n";
    
    auto sub_action_names = sub_actions();
    
    // Calculate max width for command names (for alignment)
    size_t max_width = 0;
    std::vector<std::pair<std::string, std::string>> commands_and_briefs;
    
    for (const auto& sub_name : sub_action_names) {
        if (ActionRegistry::has_action(sub_name)) {
            auto sub_action = ActionRegistry::get_action(sub_name);
            std::string display_name = sub_name.substr(name_.length() + 1); // Remove "fs." prefix
            std::string brief = sub_action.brief();
            
            commands_and_briefs.emplace_back(display_name, brief);
            max_width = std::max(max_width, display_name.length());
        }
    }
    
    // Ensure reasonable minimum spacing
    max_width = std::max(max_width, static_cast<size_t>(8));
    
    // Print commands with aligned descriptions
    for (const auto& [command, brief] : commands_and_briefs) {
        oss << "  " << std::left << std::setw(max_width + 2) << command << brief << "\n";
    }
    
    // Add General Options section for filesystem containers
    if (name_ == "fs") {
        oss << "\nGeneral Options:\n";
        oss << "  --help            Show this help message\n";
        oss << "  --rootfs <path>   Path to the root filesystem (required for all subcommands)\n";
    }
    
    oss << "\nUse '" << program::Context::program_name() << " " << name_ << ".SUBCOMMAND --help' for help on specific commands.\n";
    
    return oss.str();
}

std::string Action::generate_action_help() const {
    std::ostringstream oss;
    oss << "Usage: " << program::Context::program_name() << " " << name_;
    if (!usage_pattern_.empty()) {
        oss << " " << usage_pattern_;
    }
    oss << "\n\n" << brief_ << "\n";
    if (!description_.empty() && description_ != brief_) {
        oss << "\n" << description_ << "\n";
    }

    if (config_factory_) {
        auto options = config_factory_()->options();
        if (!options.empty()) {
            size_t width = 0;
            for (const auto& option : options) {
                width = std::max(width, option.name.size() + option.arg_name.size() + 1);
            }
            oss << "\nOptions:\n";
            for (const auto& option : options) {
                std::string signature = option.name;
                if (!option.arg_name.empty()) signature += " " + option.arg_name;
                oss << "  " << std::left << std::setw(width + 2) << signature;
                if (!option.description_lines.empty()) oss << option.description_lines.front();
                oss << "\n";
            }
        }
    }
    return oss.str();
}

// ActionRegistry implementation
std::unordered_map<std::string, Action>& ActionRegistry::get_registry() {
    static std::unordered_map<std::string, Action> registry;
    return registry;
}

void ActionRegistry::register_action(
    const std::string& name,
    std::function<int(const ToBeParsedArgs&)> handler,
    const std::string& brief,
    const std::string& description,
    const std::string& usage_pattern,
    Action::ConfigFactory config_factory) {
    
    auto& registry = get_registry();
    if (registry.find(name) != registry.end()) {
        throw std::runtime_error("Action '" + name + "' is already registered");
    }
    
    registry.emplace(name, Action(name, handler, brief, description, usage_pattern, config_factory));
}

bool ActionRegistry::has_action(const std::string& name) {
    return get_registry().find(name) != get_registry().end();
}

Action ActionRegistry::get_action(const std::string& name) {
    auto& registry = get_registry();
    auto it = registry.find(name);
    if (it == registry.end()) {
        throw std::runtime_error("Action '" + name + "' not found");
    }
    return it->second;
}

std::vector<Action> ActionRegistry::get_sub_actions(const std::string& container) {
    std::vector<Action> sub_actions;
    auto sub_names = find_sub_actions(container);
    
    for (const auto& name : sub_names) {
        if (has_action(name)) {
            sub_actions.push_back(get_action(name));
        }
    }
    
    return sub_actions;
}

std::vector<std::string> ActionRegistry::list_actions() {
    std::vector<std::string> actions;
    for (const auto& pair : get_registry()) {
        actions.push_back(pair.first);
    }
    std::sort(actions.begin(), actions.end());
    return actions;
}

int ActionRegistry::execute(const std::string& name, const ToBeParsedArgs& args) {
    if (!has_action(name)) {
        throw std::runtime_error("Action '" + name + "' not found");
    }
    
    auto action = get_action(name);
    return action.execute(args);
}

std::vector<std::string> ActionRegistry::find_sub_actions(const std::string& container) {
    std::vector<std::string> sub_actions;
    std::string prefix = container + ".";
    
    for (const auto& pair : get_registry()) {
        if (pair.first.substr(0, prefix.length()) == prefix) {
            sub_actions.push_back(pair.first);
        }
    }
    
    std::sort(sub_actions.begin(), sub_actions.end());
    return sub_actions;
}

} // namespace actions
