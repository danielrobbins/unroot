#pragma once
#include <string>
#include <vector>

// Built-in wrapper entry: when argv[0] ends with "wrapper" we call this.
// This function never returns (execv or _exit).
[[noreturn]] void runWrapper(int argc, char** argv);

// Install/copy the current unroot executable as the wrapper binary at
// ROOT/.unroot/bin/wrapper (creating directories). Returns true on success.
bool installSelfAsWrapper(const std::string& rootPath);
