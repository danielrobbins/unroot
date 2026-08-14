#include "util/subid.hpp"
#include "util/subid_backend.hpp"
#include "util/idmap.hpp"
#include "util/archive_inspector.hpp"
#include "util/archive_engine.hpp"
#include "archive_report.hpp"
#include "../build/version.hpp"

#include <charconv>
#include <clocale>
#include <climits>
#include <iostream>
#include <string>

namespace {

void usage() {
    std::cerr << "Usage: unroot-util idmap --count COUNT\n"
                 "       unroot-util idmap --validate UID_START GID_START COUNT\n"
                 "       unroot-util archive inspect --fd FD\n"
                 "       unroot-util archive unpack --fd FD\n"
                 "       unroot-util archive pack --fd FD --filter FILTER\n"
                 "       unroot-util archive --version\n";
}

bool parseId(const char* text, unsigned int& value) {
    const char* end = text + std::char_traits<char>::length(text);
    auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

int archiveCommand(int argc, char** argv) {
    if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
        std::setlocale(LC_CTYPE, "C.utf8");
    if (argc == 3 && std::string(argv[2]) == "--version") {
        const std::string version = util::archiveLibraryVersion();
        if (version.empty()) {
            std::cerr << "archive inspection requires libarchive support\n";
            return 1;
        }
        std::cout << archiveinfo::Protocol << ' ' << version << '\n';
        return 0;
    }
    unsigned int descriptor = 0;
    const std::string operation = argc >= 3 ? argv[2] : "";
    const bool force = (operation == "unpack" && argc == 6 &&
                        std::string(argv[5]) == "--force") ||
                       (operation == "pack" && argc == 8 &&
                        std::string(argv[7]) == "--force");
    if ((operation == "inspect" || operation == "unpack") &&
        (argc == 5 || force) &&
        std::string(argv[3]) == "--fd" && parseId(argv[4], descriptor) &&
        descriptor <= INT_MAX) {
        if (operation == "inspect") {
            auto result = util::inspectArchive(static_cast<int>(descriptor));
            if (!result) {
                std::cerr << result.error << '\n';
                return 1;
            }
            std::cout << archiveinfo::makeRecord(result.report) << '\n';
            return 0;
        }
        std::string error;
        const int result =
            util::unpackArchive(static_cast<int>(descriptor), force, error);
        if (!error.empty())
            std::cerr << (result == 0 ? "Warning: " : "") << error << '\n';
        return result;
    }
    if (operation == "pack" && (argc == 7 || force) &&
        std::string(argv[3]) == "--fd" && parseId(argv[4], descriptor) &&
        descriptor <= INT_MAX && std::string(argv[5]) == "--filter") {
        std::string error;
        const int result = util::packArchive(static_cast<int>(descriptor),
                                             argv[6], force, error);
        if (!error.empty())
            std::cerr << (result == 0 ? "Warning: " : "") << error << '\n';
        return result;
    }
    {
        usage();
        return 2;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << UNROOT_VERSION_STRING << '\n';
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "archive")
        return archiveCommand(argc, argv);
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
    if ((!select && !validate) || count < 1 ||
        count > util::MaxRichIdCount ||
        (validate &&
         (!util::validIdRange(uidStart, count) ||
          !util::validIdRange(gidStart, count)))) {
        std::cerr << "error: ID map values must describe ranges between 0 and "
                  << util::MaxMappedId << '\n';
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
