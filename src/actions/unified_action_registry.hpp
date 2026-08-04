#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>

namespace actions {

// Forward declarations
class ActionConfig;
struct ToBeParsedArgs;

// Clean, simple Action interface that hides all complexity
class Action {
public:
    // ConfigFactory type for creating ActionConfig instances - new simplified signature
    using ConfigFactory = std::function<std::unique_ptr<ActionConfig>()>;
    
    Action(const std::string& name,
           std::function<int(const ToBeParsedArgs&)> handler,
           const std::string& brief,
           const std::string& description,
           const std::string& usage_pattern,
           ConfigFactory config_factory = nullptr);

    // Simple public interface
    std::string name() const { return name_; }
    std::string brief() const { return brief_; }
    std::string help() const;
    bool is_container() const;
    std::vector<std::string> sub_actions() const;
    
    // Execution
    int execute(const ToBeParsedArgs& args) const;

private:
    std::string name_;
    std::string brief_;
    std::string description_;
    std::string usage_pattern_;
    std::function<int(const ToBeParsedArgs&)> handler_;
    ConfigFactory config_factory_;
    
    // Helper methods
    std::string generate_container_help() const;
    std::string generate_action_help() const;
};

// Unified registry that handles both execution and help
class ActionRegistry {
public:
    // Registration
    static void register_action(
        const std::string& name,
        std::function<int(const ToBeParsedArgs&)> handler,
        const std::string& brief,
        const std::string& description = "",
        const std::string& usage_pattern = "",
        Action::ConfigFactory config_factory = nullptr
    );

    // Simple queries
    static bool has_action(const std::string& name);
    static Action get_action(const std::string& name);
    static std::vector<Action> get_sub_actions(const std::string& container);
    static std::vector<std::string> list_actions();
    
    // Execution
    static int execute(const std::string& name, const ToBeParsedArgs& args);
    
    // Helper for finding sub-actions (used by Action class)
    static std::vector<std::string> find_sub_actions(const std::string& container);

private:
    static std::unordered_map<std::string, Action>& get_registry();
};

} // namespace actions
