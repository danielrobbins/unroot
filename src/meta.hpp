#pragma once
#include <string>
#include <utility>
#include <filesystem>
#include "nlohmann/json.hpp"
#include "util/idmap.hpp"

namespace meta {
namespace fs = std::filesystem;

struct IdMapMetaResult {
    bool found = false;
    bool created = false;
    util::IdMapPlan plan;
    std::string error;

    explicit operator bool() const { return found && error.empty(); }
};

// Read raw meta.json contents if present.
std::pair<bool, std::string> readMeta(const fs::path& env);

// Load meta.json as JSON; returns object() on absence or failure.
nlohmann::json loadMetaJson(const fs::path& rootfs);

// Atomic save (write to temp then rename); returns true on success.
bool atomicSaveMetaJson(const fs::path& rootfs, const nlohmann::json& j);

// Ensure version fields exist; returns true if upgrade (rewrite) was performed.
bool ensureVersionAndSchema(nlohmann::json& j, const fs::path& rootfs,
							int curSchema,
							int verMajor, int verMinor, int verPatch,
							const std::string& gitSha);

IdMapMetaResult readIdMap(const fs::path& rootfs);
IdMapMetaResult initializeIdMap(const fs::path& rootfs,
                                const util::IdMapPlan& plan);
IdMapMetaResult selectIdMap(util::IdMapMode mode, unsigned int richCount);
IdMapMetaResult resolveIdMap(const fs::path& rootfs, util::IdMapMode mode,
                             unsigned int richCount, bool specified);

// Update (or create) qemuWrapper section with emulator binary, args, and wrapper inode identity.
// Returns true if meta was modified and saved.
bool updateQemuWrapper(const fs::path& rootfs,
					   const std::string& emuBinary,
					   const std::vector<std::string>& emuArgs,
					   const std::string& gitSha);

} // namespace meta
