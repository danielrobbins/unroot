#pragma once
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace actions {

// Forward declaration to avoid circular dependency
struct Option;

class OptionParser {
public:
    // --- Public Types ---
    enum class Kind { Flag, Single, Multi };
    struct Entry {
        Kind kind;
        std::function<void()> flagHandler;
        std::function<void(const std::string&)> valHandler;
    };

    struct MetaEntry {
        Kind kind;
        std::vector<std::string> names;
        std::string argName;
        std::vector<std::string> description;
        bool required = false;
        bool repeatable = false;
    };

    struct PositionalMetaEntry {
        std::string name;
        std::string description;
        std::function<void(const std::string&)> handler;
        bool required = true; // Positional arguments are required by default
    };

    struct ParseResult {
        std::vector<std::string> positionals;    // Formerly preDoubleDashPositionals
        std::vector<std::string> trailing_args;   // Formerly postDoubleDash
    };

    // --- Fluent API for Defining Options ---
    OptionParser& add_flag_meta(std::initializer_list<std::string> names, const std::string& desc, std::function<void()> handler, bool required = false);
    OptionParser& add_option_meta(std::initializer_list<std::string> names, const std::string& argName, const std::string& desc, std::function<void(const std::string&)> handler, bool required = false);
    OptionParser& add_multi_option_meta(std::initializer_list<std::string> names, const std::string& argName, const std::string& desc, std::function<void(const std::string&)> handler);
    OptionParser& add_positional_meta(const std::string& name, const std::string& desc, std::function<void(const std::string&)> handler, bool required = true);

    // --- Core Methods ---
    ParseResult parse(const std::vector<std::string>& argv) const;
    std::vector<Option> exportOptions() const;
    std::string generateUsagePattern(const std::string& action_name) const;

    // Enable acceptance of trailing arguments (either via extras beyond declared
    // positional_meta_ or explicitly after a "--"). Default is disabled; when
    // disabled any detected trailing args (implicit or explicit) cause an error.
    // Returns *this for fluent configuration.
    OptionParser& allow_trailing_args(bool enable = true) { allowTrailingArgs_ = enable; return *this; }
    bool trailing_args_allowed() const { return allowTrailingArgs_; }

private:
    // --- Internal State ---
    std::unordered_map<std::string, Entry> entries_;
    std::vector<MetaEntry> meta_;
    std::vector<PositionalMetaEntry> positional_meta_;
    bool allowTrailingArgs_ = false; // opt-in flag (default: forbid trailing args)

    // --- Internal Helpers ---
    void add(std::initializer_list<std::string> names, Kind kind, std::function<void()> fh, std::function<void(const std::string&)> vh);

    // --- Internal Helper Methods ---
    void dispatchSingleOption(const Entry& e, const std::string& a, size_t& i, const std::vector<std::string>& argv) const;
    void dispatchOptionWithValue(const Entry& e, const std::string& optionName, const std::string& value) const;
    void parseShortCluster(const std::string& token, size_t& i, const std::vector<std::string>& argv) const;
};

} // namespace actions
