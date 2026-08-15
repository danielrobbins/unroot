#include "util/subid.hpp"
#include "util/subid_backend.hpp"
#include "util/idmap.hpp"
#include "util/archive_inspector.hpp"
#include "util/archive_engine.hpp"
#include "util/filesystem_probe.hpp"
#include "util/rootfs.hpp"
#include "archive_report.hpp"
#include "filesystem_caps.hpp"
#include "../build/version.hpp"

#include <charconv>
#include <clocale>
#include <climits>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cerr << "Usage: unroot-util idmap --count COUNT\n"
                 "       unroot-util idmap --validate UID_START GID_START COUNT\n"
                 "       unroot-util archive inspect --fd FD [--progress-fd FD --progress-style STYLE]\n"
                 "       unroot-util archive unpack --fd FD [--force] [--progress-fd FD --progress-style STYLE]\n"
                 "       unroot-util archive pack --fd FD --filter FILTER [--progress-fd FD --progress-style STYLE]\n"
                 "       unroot-util filesystem inspect --fd FD\n"
                 "       unroot-util injection install ...\n"
                 "       unroot-util injection restore ...\n"
                 "       unroot-util archive --version\n";
}

bool parseId(const char* text, unsigned int& value) {
    const char* end = text + std::char_traits<char>::length(text);
    auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

struct ProgressArguments {
    int descriptor = -1;
    std::optional<util::ProgressStyle> style;

    bool consume(const std::string& option, const char* value) {
        if (option == "--progress-fd") {
            unsigned int parsed = 0;
            if (descriptor >= 0 || !parseId(value, parsed) ||
                parsed <= STDERR_FILENO || parsed > INT_MAX)
                return false;
            descriptor = static_cast<int>(parsed);
            return true;
        }
        if (option == "--progress-style" && !style) {
            style = util::parseProgressStyle(value);
            return style.has_value();
        }
        return false;
    }

    bool valid() const { return (descriptor >= 0) == style.has_value(); }

    util::ProgressStyle selectedStyle() const {
        return style.value_or(util::ProgressStyle::Ascii);
    }
};

int filesystemCommand(int argc, char** argv) {
    unsigned int descriptor = 0;
    if (argc != 5 || std::string(argv[2]) != "inspect" ||
        std::string(argv[3]) != "--fd" ||
        !parseId(argv[4], descriptor) || descriptor > INT_MAX) {
        usage();
        return 2;
    }
    auto result = util::inspectFilesystem(static_cast<int>(descriptor));
    if (!result) {
        std::cerr << result.error << '\n';
        return 1;
    }
    std::cout << fsinfo::makeRecord(result.caps) << '\n';
    return 0;
}

int archiveCommand(int argc, char** argv) {
    if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
        std::setlocale(LC_CTYPE, "C.utf8");
    if (argc == 3 && std::string(argv[2]) == "--version") {
        std::cout << archiveinfo::Protocol << ' '
                  << util::archiveLibraryVersion() << '\n';
        return 0;
    }
    unsigned int descriptor = 0;
    const std::string operation = argc >= 3 ? argv[2] : "";
    if ((operation == "inspect" || operation == "unpack") &&
        argc >= 5 &&
        std::string(argv[3]) == "--fd" && parseId(argv[4], descriptor) &&
        descriptor <= INT_MAX) {
        bool force = false;
        ProgressArguments progress;
        for (int index = 5; index < argc;) {
            const std::string option = argv[index++];
            if (operation == "unpack" && option == "--force" && !force) {
                force = true;
                continue;
            }
            if (index >= argc || !progress.consume(option, argv[index++])) {
                usage();
                return 2;
            }
        }
        if (!progress.valid()) {
            usage();
            return 2;
        }
        if (operation == "inspect") {
            auto result = util::inspectArchive(static_cast<int>(descriptor),
                                               progress.descriptor,
                                               progress.selectedStyle());
            if (!result) {
                std::cerr << result.error << '\n';
                return 1;
            }
            std::cout << archiveinfo::makeRecord(result.report) << '\n';
            return 0;
        }
        std::string error;
        const int result = util::unpackArchive(
            static_cast<int>(descriptor), force, error, progress.descriptor,
            progress.selectedStyle());
        if (!error.empty())
            std::cerr << (result == 0 ? "Warning: " : "") << error << '\n';
        return result;
    }
    if (operation == "pack" && argc >= 7 &&
        std::string(argv[3]) == "--fd" && parseId(argv[4], descriptor) &&
        descriptor <= INT_MAX && std::string(argv[5]) == "--filter") {
        bool packForce = false;
        ProgressArguments progress;
        std::vector<std::string> excludes;
        std::vector<archiveio::Substitution> substitutions;
        for (int index = 7; index < argc;) {
            const std::string option = argv[index++];
            if (option == "--force") {
                if (packForce) {
                    usage();
                    return 2;
                }
                packForce = true;
                continue;
            }
            if (index >= argc) {
                usage();
                return 2;
            }
            const std::string value = argv[index++];
            if (option == "--progress-fd" || option == "--progress-style") {
                if (!progress.consume(option, value.c_str())) {
                    usage();
                    return 2;
                }
            } else if (option == "--exclude") {
                excludes.push_back(value);
            } else if (option == "--substitute") {
                const auto separator = value.find('=');
                if (separator == std::string::npos || separator == 0 ||
                    separator + 1 == value.size()) {
                    usage();
                    return 2;
                }
                substitutions.push_back(
                    {value.substr(0, separator), value.substr(separator + 1)});
            } else {
                usage();
                return 2;
            }
        }
        if (!progress.valid()) {
            usage();
            return 2;
        }
        std::string error;
        const int result = util::packArchive(static_cast<int>(descriptor),
                                             argv[6], excludes, substitutions,
                                             packForce, error,
                                             progress.descriptor,
                                             progress.selectedStyle());
        if (!error.empty())
            std::cerr << (result == 0 ? "Warning: " : "") << error << '\n';
        return result;
    }
    usage();
    return 2;
}

bool removeIfPresent(const util::Rootfs& root, const std::string& path) {
    if (root.remove(path)) return true;
    return errno == ENOENT;
}

int restoreInjection(const util::Rootfs& root,
                     const std::string& destination,
                     const std::string& backup,
                     const std::string& absent,
                     const std::string& original) {
    struct stat info {};
    if (original == "regular" || original == "symlink") {
        if (!root.lstat(backup, info) ||
            (original == "regular" ? !S_ISREG(info.st_mode)
                                   : !S_ISLNK(info.st_mode)))
            return 1;
    } else if (original != "absent" || !root.lstat(absent, info) ||
               !S_ISREG(info.st_mode)) {
        return 1;
    }
    if (root.lstat(destination, info)) {
        if (!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode)) {
            std::cerr << "injection destination became a non-file: "
                      << destination << '\n';
            return 1;
        }
        if (!root.remove(destination)) return 1;
    } else if (errno != ENOENT) {
        return 1;
    }
    if (original != "absent") {
        if (!root.move(backup, destination)) return 1;
    } else if (!removeIfPresent(root, absent)) {
        return 1;
    }
    return 0;
}

bool preserved(const util::Rootfs& root, const std::string& backup,
               const std::string& absent, const std::string& original) {
    struct stat info {};
    if (original == "absent")
        return root.lstat(absent, info) && S_ISREG(info.st_mode);
    if (!root.lstat(backup, info)) return false;
    if (original == "regular") return S_ISREG(info.st_mode);
    return original == "symlink" && S_ISLNK(info.st_mode);
}

int injectionCommand(int argc, char** argv) {
    const std::string operation = argc >= 3 ? argv[2] : "";
    if (operation == "restore" && argc == 11 &&
        std::string(argv[3]) == "--destination" &&
        std::string(argv[5]) == "--backup" &&
        std::string(argv[7]) == "--absent" &&
        std::string(argv[9]) == "--original") {
        const std::string original = argv[10];
        if (original != "regular" && original != "symlink" &&
            original != "absent") return 2;
        return restoreInjection(util::Rootfs("."), argv[4], argv[6], argv[8],
                                original);
    }

    unsigned int descriptor = 0;
    unsigned int uid = 0;
    unsigned int gid = 0;
    unsigned int mode = 0;
    const bool regular = argc == 19 && std::string(argv[3]) == "--fd";
    const bool symlink = argc == 19 && std::string(argv[3]) == "--target";
    if (operation != "install" || argc != 19 ||
        (!regular && !symlink) ||
        (regular && (!parseId(argv[4], descriptor) || descriptor > INT_MAX)) ||
        std::string(argv[5]) != "--destination" ||
        std::string(argv[7]) != "--backup" ||
        std::string(argv[9]) != "--absent" ||
        std::string(argv[11]) != "--original" ||
        std::string(argv[13]) != "--uid" || !parseId(argv[14], uid) ||
        std::string(argv[15]) != "--gid" || !parseId(argv[16], gid) ||
        std::string(argv[17]) != "--mode" || !parseId(argv[18], mode) ||
        mode > 0777) {
        usage();
        return 2;
    }

    util::Rootfs root(".");
    if (!root) {
        std::cerr << "unable to open injection rootfs\n";
        return 1;
    }
    struct stat source {};
    if (regular && (::fstat(static_cast<int>(descriptor), &source) != 0 ||
                    !S_ISREG(source.st_mode))) {
        std::cerr << "injection source is not a regular file\n";
        return 1;
    }
    const std::string payload = argv[4];
    if (symlink && (payload.empty() || payload.front() != '/' ||
                    payload.find('\n') != std::string::npos)) {
        std::cerr << "injection symlink target is invalid\n";
        return 1;
    }
    const std::string destination = argv[6];
    const std::string backup = argv[8];
    const std::string absent = argv[10];
    const std::string original = argv[12];
    if (original != "capture" && original != "regular" &&
        original != "symlink" && original != "absent") return 2;
    if (!root.parentDirectoryExists(destination)) {
        std::cerr << "injection destination directory does not exist: "
                  << destination << '\n';
        return 1;
    }

    struct stat target {};
    errno = 0;
    if (root.lstat(destination, target)) {
        if (!S_ISREG(target.st_mode) && !S_ISLNK(target.st_mode)) {
            std::cerr << "injection destination is not a regular file, "
                         "symlink, or absent: " << destination << '\n';
            return 1;
        }
    } else if (errno != ENOENT) {
        return 1;
    }

    bool captured = false;
    if (original == "capture") {
        struct stat recovery {};
        if (root.lstat(backup, recovery) || errno != ENOENT ||
            root.lstat(absent, recovery) || errno != ENOENT) {
            std::cerr << "stale injection recovery data exists for "
                      << destination << '\n';
            return 1;
        }
        errno = 0;
        if (root.lstat(destination, target)) {
            if (!root.move(destination, backup)) return 1;
        } else if (errno == ENOENT) {
            if (!root.touch(absent)) return 1;
        } else {
            return 1;
        }
        captured = true;
    } else if (!preserved(root, backup, absent, original)) {
        std::cerr << "preserved original is missing for " << destination << '\n';
        return 1;
    }

    const bool installed = regular
        ? root.copyFileAtomic(static_cast<int>(descriptor), destination,
                              static_cast<uid_t>(uid), static_cast<gid_t>(gid),
                              static_cast<mode_t>(mode))
        : root.symlinkAtomic(payload, destination, static_cast<uid_t>(uid),
                             static_cast<gid_t>(gid));
    if (installed)
        return 0;
    if (captured) {
        const std::string capturedType = root.lstat(backup, target)
            ? (S_ISLNK(target.st_mode) ? "symlink" : "regular")
            : "absent";
        (void)restoreInjection(root, destination, backup, absent, capturedType);
    }
    std::cerr << "unable to install injection at " << destination << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << UNROOT_VERSION_STRING << '\n';
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "archive")
        return archiveCommand(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "filesystem")
        return filesystemCommand(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "injection")
        return injectionCommand(argc, argv);
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
