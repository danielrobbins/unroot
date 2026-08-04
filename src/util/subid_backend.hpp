#pragma once

#include "subid.hpp"

#include <string>
#include <sys/types.h>

namespace util {

// Resolve an allocation using explicit files. This is the portable backend
// and is exposed separately for deterministic tests.
SubIdResult resolveFileSubIds(const std::string& owner, uid_t ownerUid,
                              unsigned int requestedCount,
                              const std::string& subuidPath,
                              const std::string& subgidPath);

SubIdResult validateFileSubIds(const std::string& owner, uid_t ownerUid,
                               const SubIdAllocation& allocation,
                               const std::string& subuidPath,
                               const std::string& subgidPath);

// Resolve the current host's configured subordinate-ID source.
SubIdResult resolveHostSubIds(unsigned int requestedCount);
SubIdResult validateHostSubIds(const SubIdAllocation& allocation);

} // namespace util
