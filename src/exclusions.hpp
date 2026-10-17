#pragma once

#include <string>
#include <vector>

namespace meta { struct RootLayout; }

namespace exclusions {

inline constexpr const char* HostKeys = "/etc/ssh/ssh_host_*";

// Callers hold the rootfs archive lock; operations only change the registry.
bool list(const std::string& rootfs, std::vector<std::string>& patterns,
          std::string& error);
bool archivePatterns(const meta::RootLayout& layout,
                     std::vector<std::string>& patterns, std::string& error);
bool update(const std::string& rootfs, const std::string& operation,
            const std::vector<std::string>& patterns, std::string& error);

}  // namespace exclusions
