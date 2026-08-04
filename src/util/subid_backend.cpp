#include "subid_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef UNROOT_HAVE_LIBSUBID
#if __has_include(<shadow/subid.h>)
#include <shadow/subid.h>
#elif __has_include(<subid.h>)
#include <subid.h>
#else
#error "UNROOT_HAVE_LIBSUBID requires a libsubid development header"
#endif
#endif

namespace util {
namespace {

struct Range {
    unsigned long start;
    unsigned long count;
};

bool parseUnsigned(const std::string& text, unsigned long& value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

std::vector<Range> readRanges(const std::string& path,
                              const std::string& owner,
                              const std::string& numericOwner) {
    std::ifstream file(path);
    std::vector<Range> ranges;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t first = line.find(':');
        const size_t second = first == std::string::npos
                                  ? std::string::npos
                                  : line.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            line.find(':', second + 1) != std::string::npos) {
            continue;
        }
        const std::string entryOwner = line.substr(0, first);
        if (entryOwner != owner && entryOwner != numericOwner) continue;

        unsigned long start = 0;
        unsigned long count = 0;
        if (!parseUnsigned(line.substr(first + 1, second - first - 1), start) ||
            !parseUnsigned(line.substr(second + 1), count) || count == 0) {
            continue;
        }
        ranges.push_back({start, count});
    }
    return ranges;
}

bool selectRange(const std::vector<Range>& ranges, unsigned int requested,
                 unsigned int& selected) {
    for (const auto& range : ranges) {
        if (range.count >= requested && range.start <= UINT_MAX &&
            range.start <= UINT_MAX - (requested - 1)) {
            selected = static_cast<unsigned int>(range.start);
            return true;
        }
    }
    return false;
}

bool containsRange(const std::vector<Range>& ranges, unsigned int start,
                   unsigned int count) {
    for (const auto& range : ranges) {
        if (range.start > start || start - range.start > range.count) continue;
        if (count <= range.count - (start - range.start)) return true;
    }
    return false;
}

std::string ownerName(uid_t uid) {
    constexpr size_t maxBuffer = 1024 * 1024;
    long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t size = suggested > 0
                      ? std::min(static_cast<size_t>(suggested), maxBuffer)
                      : 16384;
    while (true) {
        std::vector<char> buffer(size);
        struct passwd entry {};
        struct passwd* result = nullptr;
        int error = ::getpwuid_r(uid, &entry, buffer.data(), buffer.size(),
                                &result);
        if (error == 0 && result && result->pw_name) return result->pw_name;
        if (error != ERANGE || size == maxBuffer) break;
        size = std::min(size * 2, maxBuffer);
    }
    return std::to_string(uid);
}

SubIdResult selectAllocation(const std::vector<Range>& uidRanges,
                             const std::vector<Range>& gidRanges,
                             const std::string& owner,
                             unsigned int requestedCount,
                             const std::string& source) {
    unsigned int uidStart = 0;
    unsigned int gidStart = 0;
    if (uidRanges.empty()) {
        return {{}, "error: no subordinate UID ranges available for " + owner};
    }
    if (gidRanges.empty()) {
        return {{}, "error: no subordinate GID ranges available for " + owner};
    }
    if (!selectRange(uidRanges, requestedCount, uidStart)) {
        return {{}, "error: no subordinate UID range can map " +
                        std::to_string(requestedCount) + " IDs for " + owner};
    }
    if (!selectRange(gidRanges, requestedCount, gidStart)) {
        return {{}, "error: no subordinate GID range can map " +
                        std::to_string(requestedCount) + " IDs for " + owner};
    }
    return {{uidStart, gidStart, requestedCount, source}, {}};
}

SubIdResult validateAllocation(const std::vector<Range>& uidRanges,
                               const std::vector<Range>& gidRanges,
                               const std::string& owner,
                               const SubIdAllocation& allocation,
                               const std::string& source) {
    if (!containsRange(uidRanges, allocation.uidStart, allocation.count)) {
        return {{}, "error: recorded subordinate UID range is no longer "
                    "assigned to " + owner};
    }
    if (!containsRange(gidRanges, allocation.gidStart, allocation.count)) {
        return {{}, "error: recorded subordinate GID range is no longer "
                    "assigned to " + owner};
    }
    SubIdAllocation validated = allocation;
    validated.source = source;
    return {std::move(validated), {}};
}

#ifdef UNROOT_HAVE_LIBSUBID
std::vector<Range> providerRanges(const std::string& owner, bool groups,
                                  bool& failed) {
    struct subid_range* provider = nullptr;
    int count = groups ? subid_get_gid_ranges(owner.c_str(), &provider)
                       : subid_get_uid_ranges(owner.c_str(), &provider);
    failed = count < 0;
    std::vector<Range> ranges;
    if (count > 0 && provider) {
        ranges.reserve(static_cast<size_t>(count));
        for (int index = 0; index < count; ++index) {
            ranges.push_back({provider[index].start, provider[index].count});
        }
    }
    std::free(provider);
    return ranges;
}
#endif

} // namespace

SubIdResult resolveFileSubIds(const std::string& owner, uid_t ownerUid,
                              unsigned int requestedCount,
                              const std::string& subuidPath,
                              const std::string& subgidPath) {
    if (requestedCount == 0 || requestedCount > 65535) {
        return {{}, "error: rich ID count must be between 1 and 65535"};
    }
    const std::string numericOwner = std::to_string(ownerUid);
    return selectAllocation(readRanges(subuidPath, owner, numericOwner),
                            readRanges(subgidPath, owner, numericOwner), owner,
                            requestedCount, "files");
}

SubIdResult validateFileSubIds(const std::string& owner, uid_t ownerUid,
                               const SubIdAllocation& allocation,
                               const std::string& subuidPath,
                               const std::string& subgidPath) {
    const std::string numericOwner = std::to_string(ownerUid);
    return validateAllocation(readRanges(subuidPath, owner, numericOwner),
                              readRanges(subgidPath, owner, numericOwner), owner,
                              allocation, "files");
}

SubIdResult resolveHostSubIds(unsigned int requestedCount) {
    if (requestedCount == 0) return {{}, "error: rich ID count cannot be zero"};
    const uid_t uid = ::getuid();
    const std::string owner = ownerName(uid);
#ifdef UNROOT_HAVE_LIBSUBID
    if (!subid_init("unroot-util", nullptr)) {
        return {{}, "error: unable to initialize host subordinate-ID provider"};
    }
    bool uidFailed = false;
    bool gidFailed = false;
    auto uidRanges = providerRanges(owner, false, uidFailed);
    auto gidRanges = providerRanges(owner, true, gidFailed);
    if (uidFailed || gidFailed) {
        return {{}, "error: host subordinate-ID provider lookup failed for " + owner};
    }
    return selectAllocation(uidRanges, gidRanges, owner, requestedCount,
                            "libsubid");
#else
    return resolveFileSubIds(owner, uid, requestedCount, "/etc/subuid",
                             "/etc/subgid");
#endif
}

SubIdResult validateHostSubIds(const SubIdAllocation& allocation) {
    if (allocation.count == 0 || allocation.count > 65535 ||
        allocation.uidStart > UINT_MAX - (allocation.count - 1) ||
        allocation.gidStart > UINT_MAX - (allocation.count - 1)) {
        return {{}, "error: recorded rich ID allocation is invalid"};
    }
    const uid_t uid = ::getuid();
    const std::string owner = ownerName(uid);
#ifdef UNROOT_HAVE_LIBSUBID
    if (!subid_init("unroot-util", nullptr)) {
        return {{}, "error: unable to initialize host subordinate-ID provider"};
    }
    bool uidFailed = false;
    bool gidFailed = false;
    auto uidRanges = providerRanges(owner, false, uidFailed);
    auto gidRanges = providerRanges(owner, true, gidFailed);
    if (uidFailed || gidFailed) {
        return {{}, "error: host subordinate-ID provider lookup failed for " + owner};
    }
    return validateAllocation(uidRanges, gidRanges, owner, allocation,
                              "libsubid");
#else
    return validateFileSubIds(owner, uid, allocation, "/etc/subuid",
                              "/etc/subgid");
#endif
}

} // namespace util
