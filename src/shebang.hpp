#pragma once
#include <string>
#include <filesystem>

// Resolve the executable image the kernel will ultimately load for an ELF or
// shebang entry point. Paths are absolute within root.
std::string resolveExecutableImage(const std::filesystem::path& root,
                                   const std::string& pathInRoot);
