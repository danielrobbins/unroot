#include "archive_backend.hpp"

#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>

#include <iostream>
#include <utility>
#include <vector>

#include "linuxns.hpp"
#include "util/host_helper.hpp"

namespace archive {
namespace {
namespace fs = std::filesystem;

std::string compressionFilter(const fs::path& destination) {
  std::string name = destination.filename().string();
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  const auto endsWith = [&name](const char* suffix) {
    const size_t length = std::char_traits<char>::length(suffix);
    return name.size() >= length &&
           name.compare(name.size() - length, length, suffix) == 0;
  };
  if (endsWith(".tar.gz") || endsWith(".tgz")) return "gzip";
  if (endsWith(".tar.bz2") || endsWith(".tbz") || endsWith(".tbz2"))
    return "bzip2";
  if (endsWith(".tar.xz") || endsWith(".txz")) return "xz";
  if (endsWith(".tar.zst") || endsWith(".tzst")) return "zstd";
  if (endsWith(".tar.lz4")) return "lz4";
  if (endsWith(".tar.lzma")) return "lzma";
  if (endsWith(".tar.lz") || endsWith(".tlz")) return "lzip";
  if (endsWith(".tar.z") || endsWith(".taz")) return "compress";
  return "none";
}

int run(const fs::path& root, const util::IdMapPlan& idmap,
        std::vector<std::string> arguments) {
  const NsEnvVars environment{{"LC_ALL", "C"}};
  const std::string cwd = root.string();
  NsResult result = enterNamespace({}, arguments, idmap, nullptr, nullptr,
                                   &environment, &cwd);
  if (result.code == -1) {
    try {
      return std::stoi(result.msg);
    } catch (...) {
      return 1;
    }
  }
  if (result.code != 0)
    std::cerr << "archive namespace failed: " << result.msg
              << " (code=" << result.code << ")\n";
  return result.code;
}

UniqueFd childDescriptor(int descriptor) {
  return UniqueFd(::fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1));
}

}  // namespace

Backend::Backend() : helper_(util::siblingHostHelper()) {
  if (helper_.empty() || ::access(helper_.c_str(), X_OK) != 0)
    error_ = "unroot-util is not installed next to unroot";
}

int Backend::create(const fs::path& root, const util::IdMapPlan& idmap,
                    int outputDescriptor, const fs::path& destination,
                    const injections::ArchivePlan& injections,
                    bool force) const {
  UniqueFd output = childDescriptor(outputDescriptor);
  if (!output) return -1;
  std::vector<std::string> arguments{
      helper_, "archive", "pack", "--fd", std::to_string(output.get()),
      "--filter", compressionFilter(destination)};
  for (const auto& path : injections.excludes) {
    arguments.push_back("--exclude");
    arguments.push_back(path);
  }
  for (const auto& item : injections.substitutions) {
    arguments.push_back("--substitute");
    arguments.push_back(item.source + "=" + item.destination);
  }
  if (force) arguments.push_back("--force");
  return run(root, idmap, std::move(arguments));
}

int Backend::extract(const fs::path& root, const util::IdMapPlan& idmap,
                     const archiveio::Input& input, bool force) const {
  UniqueFd archive = input.duplicateForChild();
  if (!archive) return -1;
  std::vector<std::string> arguments{
      helper_, "archive", "unpack", "--fd", std::to_string(archive.get())};
  if (force) arguments.push_back("--force");
  return run(root, idmap, std::move(arguments));
}

}  // namespace archive
