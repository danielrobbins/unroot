#include "util/subid.hpp"
#include "util/subid_backend.hpp"
#include "../build/version.hpp"

#include <charconv>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::cerr << "Usage: unroot-util idmap --count COUNT\n"
                 "       unroot-util idmap --validate UID_START GID_START COUNT\n";
}

bool parseId(const char* text, unsigned int& value) {
    const char* end = text + std::char_traits<char>::length(text);
    auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << UNROOT_VERSION_STRING << '\n';
        return 0;
    }
    if (argc < 4 || std::string(argv[1]) != "idmap") {
        usage();
        return 2;
    }

    unsigned int uidStart = 0;
    unsigned int gidStart = 0;
    unsigned int count = 0;
    const bool select = argc == 4 && std::string(argv[2]) == "--count" &&
                        parseId(argv[3], count);
    const bool validate = argc == 6 && std::string(argv[2]) == "--validate" &&
                          parseId(argv[3], uidStart) &&
                          parseId(argv[4], gidStart) &&
                          parseId(argv[5], count);
    if ((!select && !validate) || count < 1 || count > 65535) {
        std::cerr << "error: ID map count must be between 1 and 65535\n";
        return 2;
    }

    util::SubIdResult result = validate
        ? util::validateHostSubIds({uidStart, gidStart, count, {}})
        : util::resolveHostSubIds(count);
    if (!result) {
        std::cerr << result.error << '\n';
        return 1;
    }
    const auto& allocation = result.allocation;
    std::cout << util::SubIdProtocol << ' ' << allocation.uidStart << ' '
              << allocation.gidStart << ' ' << allocation.count << ' '
              << allocation.source << '\n';
    return 0;
}
