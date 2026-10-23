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
#include "util/progress_output.hpp"

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

void deviceArguments(std::vector<std::string>& arguments,
                     const archiveio::DeviceTree& devices, int descriptor) {
  if (devices.path.empty()) return;
  arguments.insert(arguments.end(), {"--dev-path", devices.path});
  if (descriptor >= 0)
    arguments.insert(arguments.end(), {"--dev-fd", std::to_string(descriptor)});
}

std::string helperError(util::HostHelperResult result,
                        const std::string& fallback) {
  if (!result.error.empty()) return std::move(result.error);
  if (result.truncated) return "oversized response from unroot-util";
  while (!result.output.empty() &&
         std::isspace(static_cast<unsigned char>(result.output.back())))
    result.output.pop_back();
  return result.output.empty() ? fallback : std::move(result.output);
}

}  // namespace

Backend::Backend() : helper_(util::siblingHostHelper()) {
  if (helper_.empty() || ::access(helper_.c_str(), X_OK) != 0)
    error_ = "unroot-util is not installed next to unroot";
}

int Backend::create(const fs::path& root, const util::IdMapPlan& idmap,
                    int outputDescriptor, const fs::path& destination,
                    const injections::ArchivePlan& injections,
                    bool force, const archiveio::DeviceTree& devices) const {
  UniqueFd output = childDescriptor(outputDescriptor);
  if (!output) return -1;
  UniqueFd dev(devices.descriptor >= 0 ? childDescriptor(devices.descriptor)
                                      : UniqueFd{});
  if (devices.descriptor >= 0 && !dev) return -1;
  std::vector<std::string> arguments{
      helper_, "archive", "pack", "--fd", std::to_string(output.get()),
      "--filter", compressionFilter(destination)};
  deviceArguments(arguments, devices, dev.get());
  if (idmap.mode == util::IdMapMode::Single)
    arguments.push_back("--root-only");
  for (const auto& path : injections.excludes) {
    arguments.push_back("--exclude");
    arguments.push_back(path);
  }
  for (const auto& pattern : injections.patterns) {
    arguments.push_back("--exclude-glob");
    arguments.push_back(pattern.substr(1));
  }
  for (const auto& item : injections.substitutions) {
    arguments.push_back("--substitute");
    arguments.push_back(item.source + "=" + item.destination);
  }
  if (force) arguments.push_back("--force");
  util::ProgressOutput progress;
  progress.appendTo(arguments);
  return run(root, idmap, std::move(arguments));
}

int Backend::extract(const fs::path& root, const util::IdMapPlan& idmap,
                     const archiveio::Input& input, bool force,
                     const archiveio::DeviceTree& devices,
                     const std::string& rootPrefix) const {
  UniqueFd archive = input.duplicateForChild();
  if (!archive) return -1;
  UniqueFd dev(devices.descriptor >= 0 ? childDescriptor(devices.descriptor)
                                      : UniqueFd{});
  if (devices.descriptor >= 0 && !dev) return -1;
  std::vector<std::string> arguments{
      helper_, "archive", "unpack", "--fd", std::to_string(archive.get())};
  if (!rootPrefix.empty())
    arguments.insert(arguments.end(), {"--root-prefix", rootPrefix});
  deviceArguments(arguments, devices, dev.get());
  if (force) arguments.push_back("--force");
  util::ProgressOutput progress;
  progress.appendTo(arguments);
  return run(root, idmap, std::move(arguments));
}

int Backend::applyLayer(const fs::path& root, const util::IdMapPlan& idmap,
                        const archiveio::Input& input, bool force) const {
  UniqueFd archive = input.duplicateForChild();
  if (!archive) return -1;
  std::vector<std::string> arguments{
      helper_, "archive", "apply-layer", "--fd",
      std::to_string(archive.get()), "--dev-path", "dev"};
  if (force) arguments.push_back("--force");
  util::ProgressOutput progress;
  progress.appendTo(arguments);
  return run(root, idmap, std::move(arguments));
}

bool Backend::stageOci(const archiveio::Input& input,
                       const fs::path& destination,
                       std::string& error) const {
  UniqueFd archive = input.duplicateForChild();
  if (!archive) {
    error = "unable to prepare OCI image for staging";
    return false;
  }
  auto result = util::runHostHelper(
      {"archive", "stage-oci", "--fd", std::to_string(archive.get())},
      64 * 1024, helper_, destination.string());
  if (result) return true;
  error = helperError(std::move(result), "unroot-util could not stage OCI image");
  return false;
}

bool Backend::decompressLayer(const archiveio::Input& input,
                              int outputDescriptor,
                              const std::string& expectedFilter,
                              std::string& error) const {
  UniqueFd archive = input.duplicateForChild();
  UniqueFd output = childDescriptor(outputDescriptor);
  if (!archive || !output) {
    error = "unable to prepare OCI layer for decompression";
    return false;
  }
  auto result = util::runHostHelper(
      {"archive", "decompress", "--fd", std::to_string(archive.get()),
       "--output-fd", std::to_string(output.get()), "--filter",
       expectedFilter},
      64 * 1024, helper_);
  if (result) return true;
  error = helperError(std::move(result),
                      "unroot-util could not decompress OCI layer");
  return false;
}

}  // namespace archive
