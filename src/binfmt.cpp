// binfmt.cpp - helpers for Linux binfmt_misc registration and probing
#include "binfmt.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sys/mount.h>
#include <unistd.h>
#include <string>
#include "util/io.hpp"
#include "util/rootfs.hpp"

namespace {
static std::string toHex(const unsigned char* data, size_t n) {
  static const char* hexd = "0123456789abcdef";
  std::string s; s.resize(n*2);
  for (size_t i=0;i<n;++i) { s[2*i]=hexd[(data[i]>>4)&0xF]; s[2*i+1]=hexd[data[i]&0xF]; }
  return s;
}

static std::string escapedHex(const std::string& hex) {
  std::string result;
  result.reserve(hex.size() * 2);
  for (size_t offset = 0; offset + 1 < hex.size(); offset += 2) {
    result += "\\x";
    result.append(hex, offset, 2);
  }
  return result;
}

static std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

struct Handler {
  bool enabled = false;
  bool fixed = false;
  std::string magic;
  std::string mask;
};

static Handler readHandler(const std::filesystem::path& path) {
  Handler handler;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    if (line == "enabled") handler.enabled = true;
    else if (line.rfind("flags: ", 0) == 0)
      handler.fixed = line.substr(7).find('F') != std::string::npos;
    else if (line.rfind("magic ", 0) == 0) handler.magic = lower(line.substr(6));
    else if (line.rfind("mask ", 0) == 0) handler.mask = lower(line.substr(5));
  }
  return handler;
}

static bool matches(const Handler& handler, const std::string& magic,
                    const std::string& mask) {
  return handler.enabled && handler.magic == lower(magic) &&
         handler.mask == lower(mask);
}
}

static std::pair<std::string,std::string> readElfMagicAndMask(int fd) {
  std::pair<std::string,std::string> r; // magic, mask
  if (fd < 0 || ::lseek(fd, 0, SEEK_SET) == -1) return r;
  unsigned char buf[24]{}; ssize_t n = ::read(fd, buf, sizeof(buf));
  if (n == 24) {
    r.first = toHex(buf, 24);
    unsigned char m[24] = {0};
    m[0]=0xFF; m[1]=0xFF; m[2]=0xFF; m[3]=0xFF; // 0x7f E L F
    m[4]=0xFF; m[5]=0xFF; m[6]=0xFF;            // EI_CLASS, EI_DATA, EI_VERSION
    m[18]=0xFF; m[19]=0xFF;                     // e_machine
    m[20]=0xFF; m[21]=0xFF; m[22]=0xFF; m[23]=0xFF; // e_version
    r.second = toHex(m, 24);
  }
  return r;
}

std::pair<std::string, std::string> readElfMagicAndMaskAtRoot(
    const std::string& rootfs, const std::string& pathInRootfs) {
  util::Rootfs root(rootfs);
  UniqueFd fd = root.resolvedFile(pathInRootfs);
  return readElfMagicAndMask(fd.get());
}

std::string binfmtRegistration(const std::string& name,
                               const std::string& interpreter,
                               const std::string& magic,
                               const std::string& mask,
                               const std::string& flags) {
  return ":" + name + ":M::" + escapedHex(magic) + ":" + escapedHex(mask) +
         ":" + interpreter + ":" + flags;
}

HostBinfmtResult ensureHostBinfmt(const std::string& name,
                                  const std::string& emulator,
                                  const std::string& magic,
                                  const std::string& mask,
                                  const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::error_code error;
  if (!fs::exists(root / "register")) {
    if (root != fs::path("/proc/sys/fs/binfmt_misc"))
      return {false, {}, "binfmt_misc register file is unavailable"};
    fs::create_directories(root, error);
    if ((error && !fs::exists(root)) ||
        (::mount("binfmt_misc", root.c_str(), "binfmt_misc", 0, nullptr) != 0 &&
         errno != EBUSY) ||
        !fs::exists(root / "register"))
      return {false, {}, "unable to mount host binfmt_misc"};
  }

  std::string nonfixed;
  for (fs::directory_iterator entry(root, error), end;
       !error && entry != end; entry.increment(error)) {
    const std::string candidate = entry->path().filename().string();
    if (candidate == "register" || candidate == "status") continue;
    const Handler handler = readHandler(entry->path());
    if (matches(handler, magic, mask)) {
      if (handler.fixed) return {true, candidate, {}};
      nonfixed = candidate;
    }
  }
  if (error) return {false, {}, "unable to inspect host binfmt_misc"};
  if (!nonfixed.empty())
    return {false, {}, "host binfmt_misc handler '" + nonfixed +
                           "' cannot be used across chroot (missing F flag)"};
  if (fs::exists(root / name))
    return {false, {}, "host binfmt_misc handler '" + name +
                           "' exists with incompatible settings"};
  if (emulator.empty())
    return {false, {}, "no compatible host binfmt_misc handler is registered"};

  const std::string registration =
      binfmtRegistration(name, emulator, magic, mask, "F") + "\n";
  UniqueFd output(::open((root / "register").c_str(), O_WRONLY | O_CLOEXEC));
  if (!output || !util::write_all(output.get(), registration.data(),
                                  registration.size()))
    return {false, {}, "unable to register host binfmt_misc handler '" + name +
                           "'"};
  return {false, name, {}};
}
