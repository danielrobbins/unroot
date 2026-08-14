#include "hostcaps.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "archive_report.hpp"
#include "util/host_helper.hpp"
#include "util/path.hpp"

namespace {

Capability available(std::string detail = {}) {
  return {CapabilityStatus::Available, std::move(detail)};
}

Capability unavailable(std::string detail) {
  return {CapabilityStatus::Unavailable, std::move(detail)};
}

Capability unknown(std::string detail) {
  return {CapabilityStatus::Unknown, std::move(detail)};
}

Capability executable(const char* name) {
  const std::string path = util::findOnPath(name);
  return path.empty() ? unavailable(std::string(name) + " was not found on PATH")
                      : available(path);
}

Capability unshareProbe(int flags) {
  int result[2];
  if (::pipe2(result, O_CLOEXEC) != 0)
    return unknown("unable to create probe pipe");
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(result[0]);
    int error = 0;
    if (::unshare(flags) != 0) error = errno;
    (void)::write(result[1], &error, sizeof(error));
    _exit(error == 0 ? 0 : 1);
  }
  ::close(result[1]);
  if (child < 0) {
    ::close(result[0]);
    return unknown("unable to start namespace probe");
  }
  int error = 0;
  const ssize_t count = ::read(result[0], &error, sizeof(error));
  ::close(result[0]);
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || count != sizeof(error))
    return unknown("namespace probe did not complete");
  return error == 0 ? available()
                    : unavailable(std::string(::strerror(error)));
}

Capability helperCapability() {
  const std::string helper = util::siblingHostHelper();
  return !helper.empty() && ::access(helper.c_str(), X_OK) == 0
             ? available(helper)
             : unavailable("unroot-util is not installed next to unroot");
}

Capability archiveCapability(const Capability& helper) {
  if (!helper.available()) return unavailable(helper.detail);
  auto result = util::runHostHelper({"archive", "--version"}, 1024);
  while (!result.output.empty() &&
         (result.output.back() == '\n' || result.output.back() == '\r'))
    result.output.pop_back();
  if (!result.error.empty()) return unknown(result.error);
  if (result.truncated) return unknown("oversized unroot-util response");
  if (result.code != 0)
    return unavailable(result.output.empty() ? "libarchive support is unavailable"
                                             : std::move(result.output));
  const std::string prefix = std::string(archiveinfo::Protocol) + " ";
  if (result.output.compare(0, prefix.size(), prefix) != 0)
    return unknown("incompatible unroot-util archive protocol");
  return available(result.output.substr(prefix.size()));
}

Capability pathCapability(const char* path, const char* description) {
  struct stat info {};
  return ::stat(path, &info) == 0 ? available(path)
                                  : unavailable(description);
}

}  // namespace

const char* capabilityStatusName(CapabilityStatus status) {
  switch (status) {
    case CapabilityStatus::Available:
      return "available";
    case CapabilityStatus::Unavailable:
      return "unavailable";
    case CapabilityStatus::Unknown:
      return "unknown";
  }
  return "unknown";
}

nlohmann::json Capability::toJson() const {
  return {{"status", capabilityStatusName(status)}, {"detail", detail}};
}

HostCaps::HostCaps() {
  struct utsname host {};
  if (::uname(&host) == 0) {
    kernelRelease_ = host.release;
    machine_ = host.machine;
  }
  userNamespaces_ = unshareProbe(CLONE_NEWUSER);
  mountNamespaces_ = userNamespaces_.available()
                         ? unshareProbe(CLONE_NEWUSER | CLONE_NEWNS)
                         : unshareProbe(CLONE_NEWNS);
  binfmtMisc_ = pathCapability("/proc/sys/fs/binfmt_misc",
                               "binfmt_misc is not mounted");
  setgroups_ = pathCapability("/proc/self/setgroups",
                              "/proc/self/setgroups is unavailable");
  newuidmap_ = executable("newuidmap");
  newgidmap_ = executable("newgidmap");
  hostHelper_ = helperCapability();
  archiveEngine_ = archiveCapability(hostHelper_);
}

nlohmann::json HostCaps::toJson() const {
  return {{"kernel", {{"release", kernelRelease_}, {"machine", machine_}}},
          {"namespaces",
           {{"user", userNamespaces_.toJson()},
            {"mount", mountNamespaces_.toJson()},
            {"binfmt_misc", binfmtMisc_.toJson()},
            {"setgroups", setgroups_.toJson()}}},
          {"helpers",
           {{"newuidmap", newuidmap_.toJson()},
            {"newgidmap", newgidmap_.toJson()},
            {"unroot_util", hostHelper_.toJson()}}},
          {"archives", {{"libarchive", archiveEngine_.toJson()}}}};
}

const HostCaps& getGlobalHostCaps() {
  static const HostCaps capabilities;
  return capabilities;
}
