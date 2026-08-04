#include "subid.hpp"

#include "util/fd.hpp"

#include <cerrno>
#include <climits>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace util {
namespace {

std::string siblingHelper() {
    char path[PATH_MAX];
    ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length < 0) return {};
    path[length] = '\0';
    return (std::filesystem::path(path).parent_path() / "unroot-util").string();
}

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
        protocol != SubIdProtocol || count == 0 || count > 65535 ||
        uidStart > UINT_MAX || gidStart > UINT_MAX || count > UINT_MAX ||
        uidStart > UINT_MAX - (count - 1) ||
        gidStart > UINT_MAX - (count - 1)) {
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
    const std::string helper = helperPath.empty() ? siblingHelper() : helperPath;
    if (helper.empty() || ::access(helper.c_str(), X_OK) != 0) {
        return {{}, "error: rich ID mapping requires unroot-util installed next to unroot"};
    }

    int output[2];
    if (::pipe(output) != 0) {
        return {{}, "error: unable to communicate with unroot-util"};
    }
    UniqueFd readEnd(output[0]);
    UniqueFd writeEnd(output[1]);

    pid_t child = ::fork();
    if (child == 0) {
        readEnd.reset();
        int fd = writeEnd.release();
        if ((fd != STDOUT_FILENO && ::dup2(fd, STDOUT_FILENO) < 0) ||
            (fd != STDERR_FILENO && ::dup2(fd, STDERR_FILENO) < 0)) {
            _exit(126);
        }
        if (fd > STDERR_FILENO) ::close(fd);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        ::execv(helper.c_str(), argv.data());
        _exit(127);
    }
    writeEnd.reset();
    if (child < 0) return {{}, "error: unable to start unroot-util"};

    std::string captured;
    bool truncated = false;
    char buffer[512];
    while (true) {
        ssize_t length = ::read(readEnd.get(), buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) continue;
        if (length <= 0) break;
        constexpr size_t limit = 4096;
        size_t available = captured.size() < limit ? limit - captured.size() : 0;
        size_t append = static_cast<size_t>(length);
        if (append > available) {
            append = available;
            truncated = true;
        }
        captured.append(buffer, append);
    }
    readEnd.reset();

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) return {{}, "error: unable to wait for unroot-util"};

    captured = trim(captured);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!captured.empty() && !truncated) return {{}, captured};
        return {{}, "error: unroot-util failed to resolve rich ID mapping"};
    }
    if (truncated) return {{}, "error: oversized response from unroot-util"};
    return parseResponse(captured);
}

} // namespace

SubIdResult querySubIdAllocation(unsigned int requestedCount,
                                 const std::string& helperPath) {
    if (requestedCount == 0 || requestedCount > 65535) {
        return {{}, "error: rich ID count must be between 1 and 65535"};
    }

    auto result = runHelper({"unroot-util", "idmap", "--count",
                             std::to_string(requestedCount)}, helperPath);
    if (result && result.allocation.count != requestedCount)
        return {{}, "error: invalid response from unroot-util"};
    return result;
}

SubIdResult validateSubIdAllocation(const SubIdAllocation& allocation,
                                    const std::string& helperPath) {
    if (allocation.count == 0 || allocation.count > 65535 ||
        allocation.uidStart > UINT_MAX - (allocation.count - 1) ||
        allocation.gidStart > UINT_MAX - (allocation.count - 1)) {
        return {{}, "error: recorded rich ID allocation is invalid"};
    }
    auto result = runHelper(
        {"unroot-util", "idmap", "--validate",
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
