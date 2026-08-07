#include "emulation.hpp"

#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

#include "../build/version.hpp"
#include "app_exception.hpp"
#include "binfmt.hpp"
#include "compat_blacklist.hpp"
#include "meta.hpp"
#include "util/error_map.hpp"
#include "util/log_host.hpp"
#include "util/path.hpp"
#include "wrapper.hpp"

namespace emulation {
namespace {
namespace fs = std::filesystem;

bool trustedParents(fs::path path) {
  for (path = path.parent_path(); !path.empty(); path = path.parent_path()) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != 0 || (status.st_mode & 0022) != 0)
      return false;
    if (path == path.root_path()) break;
  }
  return true;
}

bool trustedExecutable(const std::string& path) {
  struct stat status {};
  return !path.empty() && path.front() == '/' &&
         ::stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_uid == 0 && (status.st_mode & 0022) == 0 &&
         ::access(path.c_str(), X_OK) == 0 && trustedParents(path) &&
         isStaticElf(path);
}

std::string trustedEmulator(const std::string& requested,
                            const std::string& fallback) {
  if (!requested.empty()) {
    std::error_code error;
    fs::path resolved = fs::weakly_canonical(requested, error);
    return !error && trustedExecutable(resolved.string()) ? resolved.string()
                                                          : std::string{};
  }
  for (const char* directory : {"/usr/local/bin", "/usr/bin", "/bin",
                                "/usr/libexec"}) {
    for (const std::string& name : {fallback + "-static", fallback}) {
      std::error_code error;
      fs::path resolved = fs::weakly_canonical(fs::path(directory) / name, error);
      if (!error && trustedExecutable(resolved.string())) return resolved.string();
    }
  }
  return {};
}

}  // namespace

const char* policyName(Policy policy) {
  return policy == Policy::Never ? "never" : "auto";
}

EmuPlan Manager::prepare() {
  if (settings_.hostArch.empty() || settings_.targetArch.empty()) {
    if (!settings_.qemu.empty() || !settings_.cpu.empty()) {
      throw AppException(
          util::make_error(util::LibErr::Invalid, 0,
                           "cannot apply QEMU options without a recognized target ELF"),
          "usage");
    }
    return {};
  }
  if (settings_.hostArch == settings_.targetArch) {
    if (!settings_.qemu.empty() || !settings_.cpu.empty()) {
      throw AppException(
          util::make_error(util::LibErr::Invalid, 0,
                           "QEMU options require a foreign-architecture target"),
          "usage");
    }
    return {};
  }
  if (settings_.policy == Policy::Never) {
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "target architecture '" + settings_.targetArch +
                             "' requires emulation, disabled by "
                             "--emulation=never"),
        "enter");
  }
  std::string emulator = settings_.qemu.empty()
                             ? qemuUserEmulatorForArch(settings_.targetArch)
                             : settings_.qemu;
  if (emulator.empty()) {
    throw AppException(
        util::make_error(util::LibErr::NotFound, 0,
                         "no default QEMU user emulator is known for target architecture '" +
                             settings_.targetArch + "'; use --qemu"),
        "enter");
  }
  if (settings_.native && !settings_.cpu.empty()) {
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "--qemu-cpu is unavailable in native mode because "
                         "host binfmt_misc handlers are system-wide"),
        "usage");
  }
  std::string source = settings_.native
                           ? trustedEmulator(settings_.qemu, emulator)
                           : std::string{};
  if (!settings_.native && settings_.qemu.empty())
    source = util::findOnPath(emulator + "-static");
  if (!settings_.native && source.empty()) source = util::findOnPath(emulator);
  if (source.empty() || !isStaticElf(source)) {
    std::string requested = settings_.qemu.empty() ? emulator + "-static"
                                                   : emulator;
    throw AppException(
        util::make_error(util::LibErr::NotFound, 0,
                         "static QEMU user emulator '" + requested +
                             (settings_.native
                                  ? "' was not found at a trusted root-controlled path"
                                  : "' was not found or is not statically linked")),
        "enter");
  }
  ElfInfo emulatorInfo = readElfInfoAtPath(source);
  if (emulatorInfo.archName != settings_.hostArch) {
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "QEMU emulator must be executable by host architecture '" +
                             settings_.hostArch + "'"),
        "enter");
  }
  compat::earlyCheck(source);

  auto [magic, mask] =
      readElfMagicAndMaskAtRoot(settings_.root, settings_.executable);
  if (magic.empty() || mask.empty()) {
    throw AppException(
        util::make_error(util::LibErr::Invalid, 0,
                         "cannot determine target ELF format from " +
                             settings_.executable),
        "enter");
  }

  if (settings_.native) {
    auto registration = ensureHostBinfmt(
        "unroot-" + settings_.targetArch, source, magic, mask);
    if (!registration) {
      throw AppException(
          util::make_error(util::LibErr::Io, 0, registration.error), "enter");
    }
    util::hostLogStep("binfmt:host", true,
                      (registration.reused ? "reused " : "registered ") +
                          registration.handler);
    return {};
  }

  if (!installSelfAsWrapper(settings_.root)) {
    throw AppException(
        util::make_error(util::LibErr::Io, 0,
                         "failed to install the Unroot emulation wrapper"),
        "enter");
  }

  EmuPlan plan;
  plan.source = source;
  plan.target = "/.unroot/bin/" +
                std::filesystem::path(source).filename().string();
  plan.registration = binfmtRegistration(
      "unroot-" + settings_.targetArch, "/.unroot/bin/wrapper", magic, mask,
      "FOC");

  std::vector<std::string> arguments;
  if (!settings_.cpu.empty()) arguments = {"-cpu", settings_.cpu};
  if (!meta::updateQemuWrapper(settings_.root, plan.target, arguments,
                               UNROOT_GIT_SHA)) {
    throw AppException(
        util::make_error(util::LibErr::Io, 0,
                         "failed to save the emulation wrapper configuration"),
        "enter");
  }
  util::hostLogStep("emu:qemu", true, source);
  return plan;
}

}  // namespace emulation
