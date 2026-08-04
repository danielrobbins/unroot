#include "config_base.hpp"
#include "parsed_args.hpp"
#include "app_exception.hpp"
#include "util/error_map.hpp"
#include "../program_context.hpp"
#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstdlib>

namespace actions {

// Helper function to get terminal width
[[maybe_unused]] static int getTerminalWidth() {
    // First try the COLUMNS environment variable
    const char* cols_env = std::getenv("COLUMNS");
    if (cols_env) {
        int cols = std::atoi(cols_env);
        if (cols > 0) {
            return cols;
        }
    }
    
    // Then try ioctl
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    
    return 80; // Default fallback width
}

// Helper function to wrap text to specified width with proper indentation
[[maybe_unused]] static std::vector<std::string> wrapText(const std::string& text, int maxWidth, int indentSize) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string word;
    std::string currentLine;
    std::string indent(indentSize, ' ');
    
    int availableWidth = maxWidth - indentSize;
    
    while (iss >> word) {
        // Check if adding this word would exceed the line width
        if (!currentLine.empty() && static_cast<int>(currentLine.length() + 1 + word.length()) > availableWidth) {
            // Finish current line and start a new one
            lines.push_back(currentLine);
            currentLine = word;
        } else {
            // Add word to current line
            if (!currentLine.empty()) {
                currentLine += " ";
            }
            currentLine += word;
        }
    }
    
    // Add the last line if there's content
    if (!currentLine.empty()) {
        lines.push_back(currentLine);
    }
    
    // Handle empty input
    if (lines.empty()) {
        lines.push_back("");
    }
    
    return lines;
}

// ActionConfig implementation

void ActionConfig::validate() const {
    // Default implementation - subclasses can override
}

void ActionConfig::postParse(const OptionParser::ParseResult& result) {
    // Default implementation - subclasses can override if they need access to full ParseResult
    (void)result; // Suppress unused parameter warning
}

void ActionConfig::parse(const ToBeParsedArgs& args) {
    auto result = getParser().parse(args.args);
    // ParseResult is handled by the positional/option handlers in configure_parser
    // Call hook for subclasses that need access to the full ParseResult
    postParse(result);
}

std::vector<Option> ActionConfig::options() const {
    return getParser().exportOptions();
}

const OptionParser& ActionConfig::getParser() const {
    if (!parser_configured_) {
        const_cast<ActionConfig*>(this)->configure_parser();
        parser_configured_ = true;
    }
    return parser_;
}

void ActionConfig::configure_parser() {
    // Base implementation does nothing - subclasses override
}

// OptionParser implementation

OptionParser& OptionParser::add_flag_meta(std::initializer_list<std::string> names, const std::string& desc, std::function<void()> handler, bool required) {
    add(names, Kind::Flag, std::move(handler), {});
    MetaEntry me;
    me.kind = Kind::Flag;
    me.names = names;
    me.description = {desc};
    me.required = required;
    me.repeatable = false;
    meta_.push_back(std::move(me));
    return *this;
}

OptionParser& OptionParser::add_option_meta(std::initializer_list<std::string> names, const std::string& argName, const std::string& desc, std::function<void(const std::string&)> handler, bool required) {
    add(names, Kind::Single, {}, std::move(handler));
    MetaEntry me;
    me.kind = Kind::Single;
    me.names = names;
    me.argName = argName;
    me.description = {desc};
    me.required = required;
    me.repeatable = false;
    meta_.push_back(std::move(me));
    return *this;
}

OptionParser& OptionParser::add_multi_option_meta(std::initializer_list<std::string> names, const std::string& argName, const std::string& desc, std::function<void(const std::string&)> handler) {
    add(names, Kind::Multi, {}, std::move(handler));
    MetaEntry me;
    me.kind = Kind::Multi;
    me.names = names;
    me.argName = argName;
    me.description = {desc};
    me.required = false;
    me.repeatable = true;
    meta_.push_back(std::move(me));
    return *this;
}

OptionParser& OptionParser::add_positional_meta(const std::string& name, const std::string& desc, std::function<void(const std::string&)> handler, bool required) {
    PositionalMetaEntry entry;
    entry.name = name;
    entry.description = desc;
    entry.handler = std::move(handler);
    entry.required = required;
    positional_meta_.push_back(std::move(entry));
    return *this;
}

void OptionParser::add(std::initializer_list<std::string> names, Kind kind, std::function<void()> fh, std::function<void(const std::string&)> vh) {
    Entry entry{kind, std::move(fh), std::move(vh)};
    for (const auto& name : names) {
        entries_[name] = entry;
    }
}

OptionParser::ParseResult OptionParser::parse(const std::vector<std::string>& argv) const {
    ParseResult result;
    std::vector<std::string> positionals_found;
    bool saw_double_dash = false;
    
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        
        if (arg == "--") {
            saw_double_dash = true;
            // Remaining args are trailing arguments (may be empty -> error later if policy requires at least one when -- used)
            for (size_t j = i + 1; j < argv.size(); ++j) {
                result.trailing_args.push_back(argv[j]);
            }
            break; // stop processing further tokens
        }
        
        if (arg.size() > 1 && arg[0] == '-') {
            if (arg.size() > 2 && arg[1] != '-') {
                // Short option cluster like -pr or -m755
                parseShortCluster(arg, i, argv);
            } else {
                // Long option: check for --foo=bar format
                std::string key = arg;
                std::string value;
                bool hasEqualValue = false;
                
                size_t equalPos = arg.find('=');
                if (equalPos != std::string::npos && equalPos > 2) {
                    // Found --foo=bar format
                    key = arg.substr(0, equalPos);
                    value = arg.substr(equalPos + 1);
                    hasEqualValue = true;
                }
                
                auto it = entries_.find(key);
                if (it == entries_.end()) {
                    throw std::invalid_argument("Unknown option: " + key);
                }
                const Entry& e = it->second;
                
                if (hasEqualValue) {
                    dispatchOptionWithValue(e, key, value);
                } else {
                    dispatchSingleOption(e, arg, i, argv);
                }
            }
        } else {
            // positional argument
            positionals_found.push_back(arg);
        }
    }
    
    // Handle positional arguments
    size_t expected_positionals = positional_meta_.size();
    if (saw_double_dash) {
        // With explicit --, reject extras while allowing optional positionals to be absent.
        if (positionals_found.size() > expected_positionals) {
            throw std::invalid_argument("Invalid number of positional arguments (expected at most " +
                std::to_string(expected_positionals) + ", received " +
                std::to_string(positionals_found.size()) + ")");
        }
        // If -- used, empty trailing list is an error (user intended to supply trailing args)
        if (result.trailing_args.empty()) {
            throw std::invalid_argument("Expected at least one trailing argument after '--'");
        }
    } else {
        if (positionals_found.size() > expected_positionals) {
            // Treat surplus as trailing args (implicit trailing form)
            for (size_t i = expected_positionals; i < positionals_found.size(); ++i) {
                result.trailing_args.push_back(positionals_found[i]);
            }
            positionals_found.resize(expected_positionals);
        }
    }

    // Policy enforcement: if trailing args detected but not allowed -> error.
    if (!allowTrailingArgs_ && !result.trailing_args.empty()) {
        // Policy forbids trailing args; conceptually these were unexpected extra positionals
        throw std::invalid_argument("Unexpected positional argument: '" + result.trailing_args.front() + "'");
    }

    // Call positional handlers and validate required positionals
    for (size_t i = 0; i < positional_meta_.size(); ++i) {
        if (i < positionals_found.size()) {
            positional_meta_[i].handler(positionals_found[i]);
            result.positionals.push_back(positionals_found[i]);
        } else if (positional_meta_[i].required) {
            throw std::invalid_argument("Missing required positional argument: " + positional_meta_[i].name);
        }
    }
    
    return result;
}

void OptionParser::dispatchSingleOption(const Entry& e, const std::string& a, size_t& i, const std::vector<std::string>& argv) const {
    switch (e.kind) {
        case Kind::Flag:
            e.flagHandler();
            break;
        case Kind::Single: {
            if (i + 1 >= argv.size()) {
                throw std::invalid_argument("Option requires argument: " + a);
            }
            e.valHandler(argv[++i]);
            break;
        }
        case Kind::Multi: {
            if (i + 1 >= argv.size()) {
                throw std::invalid_argument("Option requires argument: " + a);
            }
            e.valHandler(argv[++i]);
            break;
        }
    }
}

void OptionParser::dispatchOptionWithValue(const Entry& e, const std::string& optionName, const std::string& value) const {
    switch (e.kind) {
        case Kind::Flag:
            throw std::invalid_argument("Flag option does not accept value: " + optionName + "=" + value);
        case Kind::Single:
            e.valHandler(value);
            break;
        case Kind::Multi:
            e.valHandler(value);
            break;
    }
}

void OptionParser::parseShortCluster(const std::string& token, size_t& i, const std::vector<std::string>& argv) const {
    // token like -pr or -m755 or -m 755
    // Start at char 1
    for (size_t pos = 1; pos < token.size(); ++pos) {
        std::string key = std::string("-") + token[pos];
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            throw std::invalid_argument("Unknown option: " + key);
        }
        const Entry& e = it->second;
        if (e.kind == Kind::Flag) {
            e.flagHandler();
            continue;
        }
        // Single or multi expecting a value
        std::string remainder;
        if (pos + 1 < token.size()) {
            remainder = token.substr(pos + 1); // immediate value e.g. -m755
        }
        if (!remainder.empty()) {
            e.valHandler(remainder);
            break; // rest consumed as value
        } else {
            // Need next argv element
            if (i + 1 >= argv.size()) {
                throw std::invalid_argument("Option requires argument: " + key);
            }
            e.valHandler(argv[++i]);
            // Continue? cluster ends because option consumed next token. Any chars after would be ambiguous.
            break;
        }
    }
}

std::string OptionParser::generateUsagePattern(const std::string& action_name) const {
    std::string pattern = action_name;
    
    // Add options placeholder
    if (!meta_.empty()) {
        pattern += " [OPTIONS]";
    }
    
    // Add positional arguments
    for (const auto& pos : positional_meta_) {
        if (pos.required) {
            pattern += " " + pos.name;
        } else {
            pattern += " [" + pos.name + "]";
        }
    }
    
    return pattern;
}

std::vector<Option> OptionParser::exportOptions() const {
    std::vector<Option> out;
    for (const auto& m : meta_) {
        std::string combined;
        for (size_t i = 0; i < m.names.size(); ++i) {
            if (i) combined += ", ";
            combined += m.names[i];
        }
        Option opt(combined, m.argName, m.description);
        opt.required = m.required;
        opt.repeatable = m.repeatable;
        out.push_back(std::move(opt));
    }
    return out;
}

// FilesystemConfig implementation

void FilesystemConfig::configure_parser() {
    // Call parent implementation first
    ActionConfig::configure_parser();
    
    // Add required --rootfs option for filesystem operations
    parser_.add_option_meta({"--rootfs"}, "<path>", 
        "Path to the root filesystem for namespace operations",
        [this](const std::string& v) { 
            this->root = v; 
        }, 
        /* required= */ true);
}

void FilesystemConfig::validate() const {
    // Call base class validation
    ActionConfig::validate();
    
    // Filesystem operations require a specific rootfs path
    if (root.empty()) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            "Filesystem operations require --rootfs parameter"), "usage");
    }
    
    // Verify the rootfs path exists
    if (!std::filesystem::exists(root)) {
        throw AppException(util::make_error(util::LibErr::Invalid, 0, 
            std::string("rootfs path does not exist: ") + root), "usage");
    }
}

} // namespace actions
