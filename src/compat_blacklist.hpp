// compat_blacklist.hpp - Host kernel / emulator compatibility checks
#pragma once

#include <string>
#include <vector>

namespace compat {

enum class Severity { Warning, Blacklist };

// Perform early environment validation using the emulator that will be registered.
// Path must point to the exact qemu-user(-static) binary chosen for host registration.
// This function will:
//  - Probe uname() for kernel release/machine
//  - Run <emulatorPath> --version (capture first line)
//  - Parse version digits into vector<int>
//  - Evaluate against internal rule table (warnings + blacklist)
//  - Emit warnings to stderr
//  - Abort with exit(72) on blacklist match unless UNROOT_SKIP_EMU_BLACKLIST is set.
// Safe to call multiple times (idempotent side effects: logging only after first invocation).
void earlyCheck(const std::string& emulatorPath);

std::string probeVersionLine(const std::string& emulatorPath);
std::vector<int> parseQemuVersionLine(const std::string& line);
bool versionPrefixMatches(const std::vector<int>& prefix, const std::vector<int>& actual);

struct EvalSummary {
	std::vector<std::string> warnings;
	bool blacklisted{false};
	std::string blacklistReason;
};

EvalSummary evaluateSynthetic(const std::string& kernelRelease,
															const std::vector<int>& qemuVersion);

} // namespace compat
