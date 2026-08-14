#include "archive_action.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>
#include <vector>

#include "app_exception.hpp"
#include "archive_backend.hpp"
#include "archive_config.hpp"
#include "archive_input.hpp"
#include "archive_inspector.hpp"
#include "filesystem_inspector.hpp"
#include "meta.hpp"
#include "injections.hpp"
#include "util/archive_lock.hpp"
#include "util/error_map.hpp"
#include "util/fd.hpp"

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

class ArchiveOutput {
 public:
  explicit ArchiveOutput(const fs::path& destination)
      : destination_(destination) {
    std::string pattern =
        (destination.parent_path() / ".unroot-archive-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* directory = ::mkdtemp(writable.data());
    if (!directory) fail("unable to create temporary archive directory");
    directory_ = directory;
    temporary_ = directory_ / destination.filename();
    descriptor_.reset(::open(temporary_.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
    if (!descriptor_) fail("unable to create temporary archive output");
  }

  ~ArchiveOutput() {
    if (!temporary_.empty()) (void)::unlink(temporary_.c_str());
    if (!directory_.empty()) (void)::rmdir(directory_.c_str());
  }

  int descriptor() const { return descriptor_.get(); }

  void publish() {
    if (::fsync(descriptor_.get()) != 0) fail("unable to sync archive output");
    descriptor_.reset();
    if (::rename(temporary_.c_str(), destination_.c_str()) != 0)
      fail("unable to publish archive output");
    temporary_.clear();
    if (::rmdir(directory_.c_str()) == 0) directory_.clear();
  }

 private:
  fs::path destination_;
  fs::path directory_;
  fs::path temporary_;
  UniqueFd descriptor_;
};

void requireMappedOwnership(const archiveinfo::ArchiveReport& report,
                            const util::IdMapPlan& idmap) {
  const uint64_t maximum = std::max(report.maxUid, report.maxGid);
  if (maximum > util::MaxMappedId)
    fail("archive ownership exceeds the Linux UID/GID domain");
  if (idmap.mode != util::IdMapMode::Rich) return;
  const uint64_t limit = util::subordinateIdCount(idmap);
  if (report.maxUid <= limit && report.maxGid <= limit) return;
  fail("archive ownership exceeds the selected rich ID map; use --id-count " +
       std::to_string(maximum) + " or unpack "
       "with --native");
}

void requireFilesystemCapabilities(const archiveinfo::ArchiveReport& report,
                                   const fs::path& root, bool force) {
  const bool needsAcl = report.acls.count != 0;
  const bool needsXattr = report.xattrs.count != 0;
  if (!needsAcl && !needsXattr) return;
  auto inspected = fsinfo::FilesystemInspector().inspect(root);
  if (!inspected)
    fail("unable to inspect destination filesystem: " + inspected.error);
  const auto require = [&](bool needed, const fsinfo::Capability& capability,
                           const char* metadata) {
    if (!needed || capability.supported) return;
    std::string message = "archive contains " + std::string(metadata) +
                          ", but the destination filesystem cannot preserve "
                          "them";
    if (!capability.detail.empty()) message += ": " + capability.detail;
    if (!force) fail(message + "; use --force to accept metadata loss");
    std::cerr << "Warning: " << message << "; continuing due to --force\n";
  };
  require(needsAcl, inspected.caps.posixAcl, "POSIX ACLs");
  require(needsXattr, inspected.caps.xattr, "extended attributes");
}

}  // namespace

int ArchiveAction::perform(const PackConfig& config) {
  const fs::path root = absolutePath(config.root);
  const fs::path archivePath = absolutePath(config.archive);
  auto archiveLock = util::acquireArchiveLock(root, false);
  if (!archiveLock) fail(archiveLock.error);
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const fs::path pinnedRoot = archiveLock.pinnedRoot();
  if (isWithin(root, archivePath))
    fail("archive destination must be outside ROOT");
  if (!fs::is_directory(archivePath.parent_path()))
    fail("archive destination directory does not exist");
  archive::Backend backend;
  if (!backend) fail(backend.error());
  ArchiveOutput output(archivePath);

  auto stored = meta::readIdMap(pinnedRoot);
  if (!stored.error.empty()) fail("idmap: " + stored.error);
  if (!stored.found)
    fail(
        "rootfs has no ID-map metadata; initialize it with enter or unpack "
        "first");
  auto idmap = resolvedMap(pinnedRoot, stored.plan.mode,
                           util::subordinateIdCount(stored.plan), false);
  auto injectionPlan = injections::archivePlan(pinnedRoot);
  if (!injectionPlan) fail(injectionPlan.error);

  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const int result = backend.create(pinnedRoot, idmap, output.descriptor(),
                                    archivePath, injectionPlan, config.force);
  if (result == -1) fail("unable to prepare archive output");
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  if (result == 0) output.publish();
  return result;
}

int ArchiveAction::perform(const UnpackConfig& config) {
  const fs::path archivePath = absolutePath(config.archive);
  const fs::path root = absolutePath(config.root);
  archive::Backend backend;
  if (!backend) fail(backend.error());
  archiveio::Input input(archivePath);
  if (!input) fail(input.error());
  if (isWithin(root, archivePath)) fail("archive source must be outside ROOT");
  auto archiveLock = util::acquireArchiveLock(root, true);
  if (!archiveLock) fail(archiveLock.error);
  const fs::path pinnedRoot = archiveLock.pinnedRoot();
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  if (rootfsHasPayload(pinnedRoot)) fail("ROOT must be empty before unpacking");
  const auto mode = config.native ? util::IdMapMode::Native
                                  : util::IdMapMode::Rich;
  auto idmap = unpackMap(pinnedRoot, mode, config.idCount,
                         config.idCountSpecified);
  auto inspection = archiveio::Inspector().inspect(input);
  if (!inspection) fail("unable to inspect archive: " + inspection.error);
  if (inspection.report.reservedMetadata)
    fail("archive contains the reserved .unroot metadata tree");
  if (inspection.report.ociLayout)
    fail("archive is an OCI image layout; unroot unpack currently expects a "
         "raw rootfs tar archive or an extracted OCI layer");
  if (inspection.report.unsafePaths.count != 0)
    fail("archive contains paths outside the rootfs");
  requireMappedOwnership(inspection.report, idmap);
  requireFilesystemCapabilities(inspection.report, pinnedRoot, config.force);
  archiveLock.preserveRoot();
  auto initialized = meta::initializeIdMap(pinnedRoot, idmap, root);
  if (!initialized) fail("idmap: " + initialized.error);
  if (initialized.plan.mode != mode)
    fail("rootfs ownership mode changed during initialization");

  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  const int result =
      backend.extract(pinnedRoot, initialized.plan, input, config.force);
  if (result == -1) fail("unable to prepare archive for extraction");
  if (!archiveLock.matchesRoot()) fail("ROOT changed during archive operation");
  if (result == 0) {
    std::string error;
    if (!injections::add(
            pinnedRoot.string(), initialized.plan,
            injections::defaults(config.disabledInjections), error))
      fail("injections: " + error);
  }
  return result;
}

}  // namespace actions
