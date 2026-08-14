#include "archive_inspector.hpp"

#include <cctype>
#include <utility>

#include "util/host_helper.hpp"

namespace archiveio {
namespace {

std::string trim(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.pop_back();
  const auto first = text.find_first_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : text.substr(first);
}

}  // namespace

InspectionResult Inspector::inspect(const Input& input) const {
  UniqueFd descriptor = input.duplicateForChild();
  if (!descriptor) return {{}, "unable to prepare archive for inspection"};
  auto response = util::runHostHelper(
      {"archive", "inspect", "--fd", std::to_string(descriptor.get())},
      archiveinfo::ResponseLimit);
  std::string output = trim(std::move(response.output));
  if (!response.error.empty()) return {{}, std::move(response.error)};
  if (response.truncated) return {{}, "oversized response from unroot-util"};
  if (response.code != 0)
    return {{}, output.empty() ? "unroot-util could not inspect archive"
                               : std::move(output)};
  auto parsed = archiveinfo::parseRecord(output);
  if (!parsed) return {{}, std::move(parsed.error)};
  return {std::move(parsed.report), {}};
}

}  // namespace archiveio
