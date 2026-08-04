#pragma once
#include <string>
#include <filesystem>

namespace program {

// Global program context initialized at startup
class Context {
public:
    static void initialize(const char* argv0);
    static const std::string& program_name();
    static const std::string& program_path();

private:
    static std::string program_name_;
    static std::string program_path_;
    static bool initialized_;
};

} // namespace program
