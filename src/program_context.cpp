#include "program_context.hpp"
#include <filesystem>

namespace program {

std::string Context::program_name_;
bool Context::initialized_ = false;

void Context::initialize(const char* argv0) {
    if (initialized_) return;
    
    // Extract just the program name (handle symlinks intelligently)
    std::filesystem::path p(argv0);
    program_name_ = p.filename().string();
    
    // Handle common symlink patterns
    if (program_name_ == "ur" || program_name_.substr(0, 6) == "unroot") {
        // Keep as-is for shortened or versioned names
    } else {
        // Default to "unroot" for unknown symlinks
        program_name_ = "unroot";
    }
    
    initialized_ = true;
}

const std::string& Context::program_name() {
    if (!initialized_) {
        // Fallback if not properly initialized
        static std::string fallback = "unroot";
        return fallback;
    }
    return program_name_;
}

} // namespace program
