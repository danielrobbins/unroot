#include "filesystem_inspector.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "util/fd.hpp"
#include "util/host_helper.hpp"

namespace fsinfo {
namespace {

std::string trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

}  // namespace

Result FilesystemInspector::inspect(const std::filesystem::path& root) const {
  UniqueFd directory(
      ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory) return {{}, "unable to open destination filesystem"};
  UniqueFd child(::fcntl(directory.get(), F_DUPFD, STDERR_FILENO + 1));
  if (!child)
    return {{}, "unable to prepare destination filesystem for inspection"};

  auto response = util::runHostHelper(
      {"filesystem", "inspect", "--fd", std::to_string(child.get())},
      ResponseLimit);
  std::string output = trim(std::move(response.output));
  if (!response.error.empty()) return {{}, std::move(response.error)};
  if (response.truncated) return {{}, "oversized unroot-util response"};
  if (response.code != 0)
    return {{}, output.empty() ? "unroot-util could not inspect filesystem"
                               : std::move(output)};
  return parseRecord(output);
}

}  // namespace fsinfo
