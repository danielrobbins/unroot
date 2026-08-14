#include "subid.hpp"

#include "util/idmap.hpp"
#include "util/host_helper.hpp"

#include <climits>
#include <sstream>
#include <string>
#include <vector>

namespace util {
namespace {

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

SubIdResult parseResponse(const std::string& output) {
    std::istringstream input(output);
    std::string protocol;
    std::string source;
    unsigned long uidStart = 0;
    unsigned long gidStart = 0;
    unsigned long count = 0;

    if (!(input >> protocol >> uidStart >> gidStart >> count >> source) ||
        protocol != SubIdProtocol || count == 0 ||
        count > MaxRichIdCount ||
        uidStart > UINT_MAX || gidStart > UINT_MAX || count > UINT_MAX ||
        !validIdRange(static_cast<unsigned int>(uidStart),
                      static_cast<unsigned int>(count)) ||
        !validIdRange(static_cast<unsigned int>(gidStart),
                      static_cast<unsigned int>(count))) {
        return {{}, "error: invalid response from unroot-util"};
    }
    input >> std::ws;
    if (!input.eof()) return {{}, "error: invalid response from unroot-util"};

    return {{static_cast<unsigned int>(uidStart),
             static_cast<unsigned int>(gidStart),
             static_cast<unsigned int>(count), source}, {}};
}

SubIdResult runHelper(const std::vector<std::string>& arguments,
                      const std::string& helperPath) {
    auto response = runHostHelper(arguments, 4096, helperPath);
    std::string captured = trim(std::move(response.output));
    if (!response.error.empty()) {
        if (response.error == "unroot-util is not installed next to unroot")
            return {{}, "error: rich ID mapping requires unroot-util installed next to unroot"};
        return {{}, "error: " + response.error};
    }
    if (response.code != 0) {
        if (!captured.empty() && !response.truncated) return {{}, captured};
        return {{}, "error: unroot-util failed to resolve rich ID mapping"};
    }
    if (response.truncated)
        return {{}, "error: oversized response from unroot-util"};
    return parseResponse(captured);
}

} // namespace

SubIdResult querySubIdAllocation(unsigned int requestedCount,
                                 const std::string& helperPath) {
    if (requestedCount == 0 || requestedCount > MaxRichIdCount) {
        return {{}, "error: rich ID count must be between 1 and " +
                        std::to_string(MaxRichIdCount)};
    }

    auto result = runHelper({"idmap", "--count",
                             std::to_string(requestedCount)}, helperPath);
    if (result && result.allocation.count != requestedCount)
        return {{}, "error: invalid response from unroot-util"};
    return result;
}

SubIdResult validateSubIdAllocation(const SubIdAllocation& allocation,
                                    const std::string& helperPath) {
    if (allocation.count == 0 || allocation.count > MaxRichIdCount ||
        !validIdRange(allocation.uidStart, allocation.count) ||
        !validIdRange(allocation.gidStart, allocation.count)) {
        return {{}, "error: recorded rich ID allocation is invalid"};
    }
    auto result = runHelper(
        {"idmap", "--validate",
         std::to_string(allocation.uidStart),
         std::to_string(allocation.gidStart),
         std::to_string(allocation.count)}, helperPath);
    if (result && (result.allocation.uidStart != allocation.uidStart ||
                   result.allocation.gidStart != allocation.gidStart ||
                   result.allocation.count != allocation.count)) {
        return {{}, "error: invalid response from unroot-util"};
    }
    return result;
}

} // namespace util
