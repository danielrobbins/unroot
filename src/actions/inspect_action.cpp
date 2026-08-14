#include "inspect_action.hpp"

#include <iomanip>
#include <iostream>
#include <string>

#include "app_exception.hpp"
#include "archive_input.hpp"
#include "archive_inspector.hpp"
#include "hostcaps.hpp"
#include "inspect_config.hpp"
#include "util/error_map.hpp"

namespace actions {
namespace {

[[noreturn]] void fail(const std::string& message) {
  throw AppException(util::make_error(util::LibErr::Invalid, 0, message),
                     "inspect");
}

void capability(const char* name, const Capability& value) {
  std::cout << "  " << std::left << std::setw(20) << name
            << capabilityStatusName(value.status);
  if (!value.detail.empty()) std::cout << "  " << value.detail;
  std::cout << '\n';
}

void renderHost(const HostCaps& host) {
  std::cout << "Host\n"
            << "  " << std::left << std::setw(20) << "kernel"
            << host.kernelRelease() << '\n'
            << "  " << std::left << std::setw(20) << "machine"
            << host.machine() << "\n\nNamespaces\n";
  capability("user", host.userNamespaces());
  capability("mount", host.mountNamespaces());
  capability("binfmt_misc", host.binfmtMisc());
  capability("setgroups", host.setgroups());
  std::cout << "\nHelpers\n";
  capability("newuidmap", host.newuidmap());
  capability("newgidmap", host.newgidmap());
  capability("unroot-util", host.hostHelper());
  std::cout << "\nArchives\n";
  capability("libarchive engine", host.archiveEngine());
}

void finding(const char* name, const archiveinfo::Finding& value) {
  if (value.count == 0) return;
  std::cout << "  " << std::left << std::setw(20) << name << value.count;
  if (!value.examples.empty()) {
    std::cout << "  ";
    for (size_t index = 0; index < value.examples.size(); ++index) {
      if (index) std::cout << ", ";
      std::cout << value.examples[index];
    }
  }
  std::cout << '\n';
}

void renderArchive(const archiveinfo::ArchiveReport& report) {
  std::cout << "Archive\n"
            << "  " << std::left << std::setw(20) << "format" << report.format
            << '\n'
            << "  " << std::left << std::setw(20) << "filters";
  if (report.filters.empty())
    std::cout << "none";
  else
    for (size_t index = 0; index < report.filters.size(); ++index) {
      if (index) std::cout << ", ";
      std::cout << report.filters[index];
    }
  std::cout << '\n'
            << "  " << std::left << std::setw(20) << "members"
            << report.members << '\n'
            << "  " << std::left << std::setw(20) << "regular bytes"
            << report.regularBytes << '\n'
            << "  " << std::left << std::setw(20) << "maximum UID"
            << report.maxUid << '\n'
            << "  " << std::left << std::setw(20) << "maximum GID"
            << report.maxGid << "\n\nLayout\n"
            << "  " << std::left << std::setw(20) << "kind"
            << (report.ociLayout ? "OCI image" : "raw filesystem") << '\n'
            << "  " << std::left << std::setw(20) << "reserved .unroot"
            << (report.reservedMetadata ? "present" : "absent")
            << "\n\nMetadata\n";
  finding("unsafe paths", report.unsafePaths);
  finding("ACL entries", report.acls);
  finding("extended attributes", report.xattrs);
  finding("file capabilities", report.capabilities);
  finding("SELinux labels", report.selinuxLabels);
  finding("sparse files", report.sparseFiles);
  finding("hard links", report.hardLinks);
  finding("symbolic links", report.symbolicLinks);
  finding("devices", report.devices);
  finding("FIFOs", report.fifos);
  finding("sockets", report.sockets);
  if (report.unsafePaths.count == 0 && report.acls.count == 0 &&
      report.xattrs.count == 0 && report.sparseFiles.count == 0 &&
      report.hardLinks.count == 0 && report.symbolicLinks.count == 0 &&
      report.devices.count == 0 && report.fifos.count == 0 &&
      report.sockets.count == 0)
    std::cout << "  none detected\n";
}

}  // namespace

int InspectAction::perform(const InspectConfig& config) {
  if (config.subject == "host") {
    const auto& host = getGlobalHostCaps();
    if (config.json)
      std::cout << host.toJson().dump(2) << '\n';
    else
      renderHost(host);
    return 0;
  }

  archiveio::Input input(config.archive);
  if (!input) fail(input.error());
  auto inspection = archiveio::Inspector().inspect(input);
  if (!inspection) fail(inspection.error);
  if (config.json)
    std::cout << inspection.report.toJson().dump(2) << '\n';
  else
    renderArchive(inspection.report);
  return 0;
}

}  // namespace actions
