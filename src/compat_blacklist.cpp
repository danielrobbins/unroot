// compat_blacklist.cpp - implementation of host kernel / qemu version compatibility checks

#include "compat_blacklist.hpp"
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <vector>
#include <optional>
#include <cctype>
#include <iostream>

namespace compat {

struct Rule {
    Severity severity;
    std::optional<std::regex> kernelPattern; // nullopt -> no kernel condition
    std::vector<int> qemuVersionPrefix;      // empty -> no qemu condition
    std::string reason;
};

struct Probe {
    std::string kernelRelease;
    std::string machine;
    std::string emulatorVersionLine; // first line of --version
    std::vector<int> emulatorVersion; // parsed ints
};

static std::vector<int> parseVersion(const std::string& line) {
    // Strategy per user request: take the last whitespace-delimited token on the line and
    // interpret it as the version string (e.g. '7.1.0'). This avoids picking up 'aarch64'.
    std::vector<int> out;
    if (line.empty()) return out;
    // Find last non-space
    size_t end = line.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return out;
    // Find start of that token
    size_t start = line.find_last_of(" \t", end);
    if (start == std::string::npos) start = 0; else start += 1;
    std::string token = line.substr(start, end - start + 1);
    // Trim common wrappers / punctuation
    auto trimPunct = [](std::string &s){
        while(!s.empty() && (s.back()==')' || s.back()==',' || s.back()==';')) s.pop_back();
        while(!s.empty() && (s.front()=='(' || s.front()=='v')) s.erase(s.begin());
    };
    trimPunct(token);
    if (token.empty()) return out;
    // Validate: digits and dots only
    for(char c: token){ if(!(std::isdigit((unsigned char)c) || c=='.')) return out; }
    // Split by '.'
    size_t pos=0; int parts=0;
    while(pos < token.size() && parts < 4){
        size_t dot = token.find('.', pos);
        std::string part = (dot==std::string::npos)? token.substr(pos) : token.substr(pos, dot-pos);
        if(part.empty()) break; // malformed
        try { out.push_back(std::stoi(part)); }
        catch(...) { out.clear(); return out; }
        ++parts;
        if(dot==std::string::npos) break;
        pos = dot + 1;
    }
    return out;
}

std::string probeVersionLine(const std::string& exePath) {
    int output[2];
    if (::pipe(output) != 0) return {};

    pid_t child = ::fork();
    if (child < 0) {
        ::close(output[0]);
        ::close(output[1]);
        return {};
    }
    if (child == 0) {
        ::close(output[0]);
        if (::dup2(output[1], STDOUT_FILENO) < 0 ||
            ::dup2(output[1], STDERR_FILENO) < 0) _exit(127);
        ::close(output[1]);
        ::execl(exePath.c_str(), exePath.c_str(), "--version", nullptr);
        _exit(127);
    }

    ::close(output[1]);
    std::string line;
    char buffer[256];
    for (;;) {
        ssize_t count = ::read(output[0], buffer, sizeof(buffer));
        if (count > 0 && line.find('\n') == std::string::npos &&
            line.size() < 512) {
            size_t available = 512 - line.size();
            line.append(buffer, static_cast<size_t>(count) < available
                                    ? static_cast<size_t>(count)
                                    : available);
        } else if (count == 0) {
            break;
        } else if (count < 0 && errno != EINTR) {
            break;
        }
    }
    ::close(output[0]);
    int status;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}

    size_t newline = line.find_first_of("\r\n");
    if (newline != std::string::npos) line.resize(newline);
    while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
    while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
    return line;
}

bool versionPrefixMatches(const std::vector<int>& prefix, const std::vector<int>& actual) {
    if (prefix.empty()) return true; // wildcard
    if (actual.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) if (actual[i] != prefix[i]) return false;
    return true;
}

static Probe probe(const std::string& emulatorPath) {
    struct utsname uts{};
    if (uname(&uts) != 0) { uts.release[0] = 0; uts.machine[0] = 0; }
    Probe p;
    p.kernelRelease = uts.release;
    p.machine = uts.machine;
    p.emulatorVersionLine = probeVersionLine(emulatorPath);
    p.emulatorVersion = parseVersion(p.emulatorVersionLine);
    return p;
}

struct EvalResult {
    std::vector<std::string> warnings;
    std::optional<std::string> blacklistReason;
};

static bool debugEnabled() {
    if (const char* e = std::getenv("UNROOT_EMU_COMPAT_DEBUG")) return *e != 0;
    if (const char* v = std::getenv("UNROOT_VERBOSE")) return *v != 0;
    return false;
}

static EvalResult evaluate(const Probe& pr, const std::vector<Rule>& rules) {
    EvalResult er;
    bool dbg = debugEnabled();
    if (dbg) {
        std::cerr << "[unroot][compat-debug] kernel='" << pr.kernelRelease << "' qemuLine='" << pr.emulatorVersionLine << "' parsedVersion=";
        if (pr.emulatorVersion.empty()) std::cerr << "<empty>"; else {
            std::cerr << '[';
            for (size_t i=0;i<pr.emulatorVersion.size();++i){ if(i) std::cerr<<','; std::cerr<<pr.emulatorVersion[i]; }
            std::cerr << ']';
        }
        std::cerr << "\n";
    }
    size_t idx = 0;
    for (const auto& r : rules) {
        bool kernelOK = !r.kernelPattern || std::regex_search(pr.kernelRelease, *r.kernelPattern);
    bool versionOK = versionPrefixMatches(r.qemuVersionPrefix, pr.emulatorVersion);
        if (kernelOK && versionOK) {
            if (r.severity == Severity::Warning) {
                er.warnings.push_back(r.reason);
            } else if (r.severity == Severity::Blacklist && !er.blacklistReason) {
                er.blacklistReason = r.reason;
            }
        }
        if (dbg) {
            std::cerr << "[unroot][compat-debug] rule#" << idx
                      << " sev=" << (r.severity==Severity::Warning?"WARN":"BLACKLIST")
                      << " kernelOK=" << (kernelOK?"1":"0")
                      << " verOK=" << (versionOK?"1":"0")
                      << " prefix=";
            if (r.qemuVersionPrefix.empty()) std::cerr << '*'; else {
                std::cerr << '['; for (size_t i=0;i<r.qemuVersionPrefix.size();++i){ if(i) std::cerr<<','; std::cerr<<r.qemuVersionPrefix[i]; } std::cerr << ']'; }
            std::cerr << " reason='" << r.reason << "'";
            if (r.kernelPattern) std::cerr << " pattern-set"; else std::cerr << " no-pattern";
            std::cerr << "\n";
        }
        ++idx;
    }
    return er;
}

// Static rule table. Adjust / extend as needed.
// Provided concrete environment from user:
//   Kernel: 6.6.87.2-microsoft-standard-WSL2
//   QEMU line: qemu-aarch64 version 7.1.0
// We'll treat this specific pair as BLACKLIST (example) and general WSL2 as WARNING.
static const std::vector<Rule> kRules = {
    { Severity::Warning, std::regex(".*microsoft-standard-WSL2$"), {}, "WSL2 kernel detected; emulation may be unreliable." },
    { Severity::Blacklist, std::regex("^6\\.6\\.87\\.2-microsoft-standard-WSL2$"), {7,1,0}, "Known unreliable combination: kernel 6.6.87.2 WSL2 with QEMU 7.1.0" }
};

void earlyCheck(const std::string& emulatorPath) {
    static bool ran = false; // prevent duplicate verbose logs if called multiple times
    auto pr = probe(emulatorPath);
    auto eval = evaluate(pr, kRules);
    if (!ran) {
        for (const auto& w : eval.warnings) {
            std::cerr << "[unroot] WARNING: " << w
                      << " (kernel=\"" << pr.kernelRelease
                      << "\" qemu=\"" << (pr.emulatorVersionLine.empty()?"?":pr.emulatorVersionLine)
                      << "\")\n";
        }
    }
    if (eval.blacklistReason) {
        bool override = false; if (const char* o = std::getenv("UNROOT_SKIP_EMU_BLACKLIST")) override = *o != 0;
        if (!override) {
            std::cerr << "[unroot] FATAL: Blacklisted emulator environment.\n"
                      << "  Kernel : " << pr.kernelRelease << "\n"
                      << "  Machine: " << pr.machine << "\n"
                      << "  QEMU   : " << (pr.emulatorVersionLine.empty()?"(unavailable)":pr.emulatorVersionLine) << "\n"
                      << "  Path   : " << emulatorPath << "\n"
                      << "  Reason : " << *eval.blacklistReason << "\n"
                      << "Set UNROOT_SKIP_EMU_BLACKLIST=1 to bypass (unsupported).\n";
            std::exit(72);
        } else if (!ran) {
            std::cerr << "[unroot] NOTICE: Blacklist override active; proceeding despite: "
                      << *eval.blacklistReason << "\n";
        }
    }
    ran = true;
}

// Test helper exports
std::vector<int> parseQemuVersionLine(const std::string& line) { return parseVersion(line); }

EvalSummary evaluateSynthetic(const std::string& kernelRelease,
                              const std::vector<int>& qemuVersion) {
    EvalSummary out; out.blacklisted = false;
    for (const auto& r : kRules) {
        bool kernelOK = !r.kernelPattern || std::regex_search(kernelRelease, *r.kernelPattern);
        bool versionOK = versionPrefixMatches(r.qemuVersionPrefix, qemuVersion);
        if (kernelOK && versionOK) {
            if (r.severity == Severity::Warning) out.warnings.push_back(r.reason);
            else if (r.severity == Severity::Blacklist && !out.blacklisted) { out.blacklisted = true; out.blacklistReason = r.reason; }
        }
    }
    return out;
}

} // namespace compat
