#pragma once

#include <string>

namespace util {

inline constexpr const char* SubIdProtocol = "unroot-idmap-v1";

struct SubIdAllocation {
    unsigned int uidStart = 0;
    unsigned int gidStart = 0;
    unsigned int count = 0;
    std::string source;
};

struct SubIdResult {
    SubIdAllocation allocation;
    std::string error;

    explicit operator bool() const { return error.empty() && allocation.count != 0; }
};

// Query the trusted unroot-util sibling for a rich host ID allocation. An
// explicit helper path is accepted for unit tests.
SubIdResult querySubIdAllocation(unsigned int requestedCount,
                                 const std::string& helperPath = {});

// Verify that an exact recorded allocation is still assigned to the current
// user. The allocation itself remains authoritative.
SubIdResult validateSubIdAllocation(const SubIdAllocation& allocation,
                                    const std::string& helperPath = {});

} // namespace util
