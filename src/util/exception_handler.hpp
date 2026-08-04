#pragma once
#include <functional>
#include <string>

namespace util {
    enum class ErrorContext {
        CLI,      // For standard user-facing commands. Prints human-readable errors to stderr.
        NDJSON    // For internal fs commands. Prints JSON errors to stdout.
    };

    // The centralized exception handler.
    int handle_exceptions(
        std::function<int()> main_logic,
        ErrorContext context,
        const std::string& command_name = "" // Optional: for NDJSON output
    );
}