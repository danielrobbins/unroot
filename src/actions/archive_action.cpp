#include "archive_action.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

#include "app_exception.hpp"
#include "archive_config.hpp"
#include "linuxns.hpp"
#include "meta.hpp"
#include "util/archive_lock.hpp"
#include "util/error_map.hpp"
#include "util/fd.hpp"
#include "util/path.hpp"
#include "util/proc.hpp"

namespace actions {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message) {
  throw AppException(util::make_error(util::LibErr::Invalid, 0, message),
                     "archive");
}

fs::path absolutePath(const fs::path& path) {
  std::error_code error;
  fs::path absolute = fs::absolute(path, error);
  if (error) fail("unable to resolve path: " + path.string());
  fs::path result = fs::weakly_canonical(absolute, error);
  if (error) fail("unable to resolve path: " + path.string());
  return result;
}

bool isWithin(const fs::path& root, const fs::path& path) {
  const fs::path relative = path.lexically_relative(root);
  if (relative.empty() || relative == ".") return true;
  return *relative.begin() != "..";
}

bool rootfsHasPayload(const fs::path& root) {
  for (const auto& entry : fs::directory_iterator(root))
    if (entry.path().filename() != ".unroot") return true;
  return false;
}

util::IdMapPlan resolvedMap(const fs::path& root, util::IdMapMode mode,
                            unsigned int count, bool specified) {
  auto result = meta::resolveIdMap(root, mode, count, specified);
  if (!result) fail("idmap: " + result.error);
  return std::move(result.plan);
}

util::IdMapPlan unpackMap(const fs::path& root, util::IdMapMode mode,
                          unsigned int count, bool specified) {
  auto stored = meta::readIdMap(root);
  if (!stored.error.empty()) fail("idmap: " + stored.error);
  if (stored.found) {
    if (stored.plan.mode != mode)
      fail("requested ownership mode differs from initialized rootfs");
    const unsigned int storedCount = util::subordinateIdCount(stored.plan);
    return resolvedMap(root, mode, specified ? count : storedCount, specified);
  }
  auto selected = meta::selectIdMap(
      mode, mode == util::IdMapMode::Rich ? count : 0);
  if (!selected) fail("idmap: " + selected.error);
  return std::move(selected.plan);
}

std::string tarPath() {
  std::string tar = util::findOnPath("tar");
  if (tar.empty()) fail("GNU tar was not found on PATH");
  return absolutePath(tar).string();
}

void validateArchive(const std::string& tar, const fs::path& archive) {
  const std::string file = "--file=" + archive.string();
  if (util::spawn_and_wait_execv(tar, {tar, "--list", file}, true) != 0)
    fail("unable to read tar archive: " + archive.string());
  for (const char* pattern :
       {".unroot", ".unroot/*", "./.unroot", "./.unroot/*"}) {
    if (util::spawn_and_wait_execv(
            tar, {tar, "--list", file, "--wildcards", "--anchored", pattern},
            true) == 0)
      fail("archive contains the reserved .unroot metadata tree");
  }
}

int runTar(const fs::path& root, const util::IdMapPlan& idmap,
           std::vector<std::string> arguments) {
  const char* path = ::getenv("PATH");
  NsEnvVars environment{{"PATH", path && *path ? path : "/usr/bin:/bin"},
                        {"LC_ALL", "C"}};
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

std::vector<std::string> metadataOptions() {
  std::vector<std::string> options{"--numeric-owner", "--acls", "--xattrs",
                                   "--xattrs-include=*"};
  if (fs::exists("/sys/fs/selinux/enforce")) options.push_back("--selinux");
  return options;
}

class MetadataProbeFile {
 public:
  MetadataProbeFile() {
    char path[] = "/tmp/.unroot-tar-probe-XXXXXX";
    UniqueFd file(::mkstemp(path));
    if (!file) fail("unable to create GNU tar metadata probe file");
    path_ = path;
  }

  ~MetadataProbeFile() {
    if (!path_.empty()) (void)::unlink(path_.c_str());
  }

  MetadataProbeFile(const MetadataProbeFile&) = delete;
  MetadataProbeFile& operator=(const MetadataProbeFile&) = delete;

  fs::path directory() const { return path_.parent_path(); }
  fs::path filename() const { return path_.filename(); }

 private:
  fs::path path_;
};

class ArchiveOutput {
 public:
  explicit ArchiveOutput(const fs::path& destination)
      : destination_(destination) {
    const std::string suffix = destination.extension().string();
    std::string pattern =
        (destination.parent_path() /
         ("." + destination.filename().string() + ".unroot-tmp-XXXXXX" +
          suffix))
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    UniqueFd file(::mkstemps(writable.data(), static_cast<int>(suffix.size())));
    if (!file) fail("unable to create temporary archive output");
    temporary_ = writable.data();
  }

  ~ArchiveOutput() {
    if (!temporary_.empty()) (void)::unlink(temporary_.c_str());
  }

  const fs::path& path() const { return temporary_; }

  void publish() {
    if (::rename(temporary_.c_str(), destination_.c_str()) != 0)
      fail("unable to publish archive output");
    temporary_.clear();
  }

 private:
  fs::path destination_;
  fs::path temporary_;
};

void requireMetadataSupport(const std::string& tar, bool force) {
  MetadataProbeFile probeFile;
  std::vector<std::string> arguments{tar, "--create", "--file=/dev/null",
                                     "--no-recursion",
                                     "--directory=" +
                                         probeFile.directory().string()};
  auto metadata = metadataOptions();
  arguments.insert(arguments.end(), metadata.begin(), metadata.end());
  arguments.push_back(probeFile.filename().string());
  auto probe = util::capture_execv(tar, arguments);
  if (!probe.output.empty() && probe.output.back() != '\n')
    probe.output.push_back('\n');
  if (probe.code != 0)
    fail("unable to verify GNU tar metadata support" +
         (probe.output.empty() ? std::string() : ":\n" + probe.output));
  if (probe.output.empty()) return;
  const std::string message =
      "GNU tar cannot preserve all requested filesystem metadata:\n" +
      probe.output;
  if (!force) fail(message + "Use --force to accept metadata loss.");
  std::cerr << "Warning: " << message
            << "Continuing because --force was specified.\n";
}

}  // namespace

int ArchiveAction::perform(const PackConfig& config) {
  const fs::path root = absolutePath(config.root);
  const fs::path archive = absolutePath(config.archive);
  auto archiveLock = util::acquireArchiveLock(root, false);
  if (!archiveLock) fail(archiveLock.error);
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const fs::path pinnedRoot = archiveLock.pinnedRoot();
  if (isWithin(root, archive)) fail("archive destination must be outside ROOT");
  if (!fs::is_directory(archive.parent_path()))
    fail("archive destination directory does not exist");
  const std::string tar = tarPath();
  requireMetadataSupport(tar, config.force);
  ArchiveOutput output(archive);

  auto stored = meta::readIdMap(pinnedRoot);
  if (!stored.error.empty()) fail("idmap: " + stored.error);
  if (!stored.found)
    fail(
        "rootfs has no ID-map metadata; initialize it with enter or unpack "
        "first");
  auto idmap = resolvedMap(pinnedRoot, stored.plan.mode,
                           util::subordinateIdCount(stored.plan), false);

  std::vector<std::string> arguments{tar,
                                     "--create",
                                     "--auto-compress",
                                     "--format=pax",
                                     "--file=" + output.path().string(),
                                     "--sparse",
                                     "--anchored",
                                     "--exclude=.unroot",
                                     "--exclude=./.unroot"};
  auto metadata = metadataOptions();
  arguments.insert(arguments.end(), metadata.begin(), metadata.end());
  arguments.push_back(".");
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const int result = runTar(pinnedRoot, idmap, std::move(arguments));
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  if (result == 0) output.publish();
  return result;
}

int ArchiveAction::perform(const UnpackConfig& config) {
  const fs::path archive = absolutePath(config.archive);
  const fs::path root = absolutePath(config.root);
  auto archiveLock = util::acquireArchiveLock(root, true);
  if (!archiveLock) fail(archiveLock.error);
  const fs::path pinnedRoot = archiveLock.pinnedRoot();
  const auto mode = config.native ? util::IdMapMode::Native
                                  : util::IdMapMode::Rich;
  auto idmap = unpackMap(pinnedRoot, mode, config.idCount,
                         config.idCountSpecified);
  const std::string tar = tarPath();
  validateArchive(tar, archive);
  if (isWithin(root, archive)) fail("archive source must be outside ROOT");
  requireMetadataSupport(tar, config.force);

  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  if (rootfsHasPayload(pinnedRoot)) fail("ROOT must be empty before unpacking");
  archiveLock.preserveRoot();
  auto initialized = meta::initializeIdMap(pinnedRoot, idmap, root);
  if (!initialized) fail("idmap: " + initialized.error);
  if (initialized.plan.mode != mode)
    fail("rootfs ownership mode changed during initialization");

  std::vector<std::string> arguments{tar,
                                     "--extract",
                                     "--file=" + archive.string(),
                                     "--same-owner",
                                     "--same-permissions",
                                     "--delay-directory-restore",
                                     "--anchored",
                                     "--exclude=.unroot",
                                     "--exclude=./.unroot"};
  auto metadata = metadataOptions();
  arguments.insert(arguments.end(), metadata.begin(), metadata.end());
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const int result = runTar(pinnedRoot, initialized.plan, std::move(arguments));
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  return result;
}

}  // namespace actions
