// binfmt.hpp - helpers for Linux binfmt_misc registration and probing
#pragma once

#include <filesystem>
#include <string>
#include <utility>

struct HostBinfmtResult {
  bool reused = false;
  std::string handler;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Read target ELF magic through rootfs-contained resolution for binfmt_misc.
std::pair<std::string, std::string> readElfMagicAndMaskAtRoot(
    const std::string& rootfs, const std::string& pathInRootfs);

std::string binfmtRegistration(const std::string& name,
                               const std::string& interpreter,
                               const std::string& magic,
                               const std::string& mask,
                               const std::string& flags);
HostBinfmtResult ensureHostBinfmt(
    const std::string& name, const std::string& emulator,
    const std::string& magic, const std::string& mask,
    const std::filesystem::path& root = "/proc/sys/fs/binfmt_misc");
