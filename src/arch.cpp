// arch.cpp — ELF/host arch detection
#include "arch.hpp"
#include <cstdint>
#include <sys/utsname.h>
#include <array>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "util/io.hpp"
#include "util/result.hpp"
#include "util/errors.hpp"
#include "util/rootfs.hpp"


// --- Helper Functions ---
static std::string normalize(const char* m) {
  std::string s = m ? std::string(m) : std::string();
  if (s == "x86_64" || s == "amd64") return "x86_64";
  if (s == "i386" || s == "i486" || s == "i586" || s == "i686") return "x86";
  if (s == "aarch64" || s == "arm64") return "arm64";
  if (s == "aarch64_be" || s == "arm64_be") return "arm64_be";
  if (s.rfind("arm",0)==0) return s.back() == 'b' ? "armeb" : "arm";
  if (s == "ppc64le" || s == "powerpc64le") return "ppc64le";
  if (s == "ppc64" || s == "powerpc64") return "ppc64";
  if (s == "riscv32") return "riscv32";
  if (s == "riscv64") return "riscv64";
  return s;
}

static uint64_t decode(const unsigned char* bytes, size_t size,
                       unsigned char data) {
  uint64_t value = 0;
  if (data == 1) {
    for (size_t i = 0; i < size; ++i)
      value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  } else if (data == 2) {
    for (size_t i = 0; i < size; ++i) value = (value << 8) | bytes[i];
  }
  return value;
}

static std::string emToArch(uint16_t em, unsigned char elfClass,
                            unsigned char data) {
  const bool elf32 = elfClass == 1;
  const bool elf64 = elfClass == 2;
  const bool little = data == 1;
  const bool big = data == 2;
  switch (em) {
    case 3: return elf32 && little ? "x86" : std::string();       // EM_386
    case 62: return (elf32 || elf64) && little ? "x86_64" : std::string(); // EM_X86_64
    case 183: // EM_AARCH64
      return elf64 && (little || big) ? (little ? "arm64" : "arm64_be")
                                      : std::string();
    case 40: // EM_ARM
      return elf32 && (little || big) ? (little ? "arm" : "armeb")
                                      : std::string();
    case 243: // EM_RISCV
      return little && (elf32 || elf64) ? (elf32 ? "riscv32" : "riscv64")
                                        : std::string();
    case 21: // EM_PPC64
      return elf64 && (little || big) ? (little ? "ppc64le" : "ppc64")
                                      : std::string();
    default: return {};
  }
}

// --- ElfParser Implementation ---

class ElfParser {
public:
    explicit ElfParser(const std::string& absPath);
    explicit ElfParser(int fd);

    bool isStatic() const { return is_static_; }
    const ElfInfo& getInfo() const { return info_; }

private:
    void parse();
    void checkStatic();

    UniqueFd fd_;
    ElfInfo info_{};
    bool is_static_ = false;
    std::array<unsigned char, 64> header_cache_{};
};

ElfParser::ElfParser(const std::string& absPath)
    : fd_(::open(absPath.c_str(), O_RDONLY | O_CLOEXEC)) {
    parse();
    if (info_.valid) checkStatic();
}

ElfParser::ElfParser(int fd) : fd_(::fcntl(fd, F_DUPFD_CLOEXEC, 0)) {
    parse();
    if (info_.valid) checkStatic();
}

void ElfParser::parse() {
  if (!fd_ || ::lseek(fd_.get(), 0, SEEK_SET) == -1) return;

  if (!util::read_exact(fd_.get(), header_cache_.data(), header_cache_.size())) return;

  if (!(header_cache_[0] == 0x7f && header_cache_[1] == 'E' && header_cache_[2] == 'L' && header_cache_[3] == 'F')) {
    return; // Not ELF
  }

  info_.valid = true;
  info_.ei_class = header_cache_[4];
  info_.ei_data  = header_cache_[5];
  info_.ei_osabi = header_cache_[7];
  info_.e_machine = static_cast<uint16_t>(decode(
      header_cache_.data() + 18, 2, info_.ei_data));
  info_.archName = emToArch(info_.e_machine, info_.ei_class, info_.ei_data);
}

void ElfParser::checkStatic() {
    if (!fd_) return;

    bool is64 = (info_.ei_class == 2);
    if (!is64 && info_.ei_class != 1) return;
    if (info_.ei_data != 1 && info_.ei_data != 2) return;

    // Define offsets for program header table info.
    constexpr off_t PHOFF_32 = 28, PHENTSIZE_32 = 42, PHNUM_32 = 44;
    constexpr off_t PHOFF_64 = 32, PHENTSIZE_64 = 54, PHNUM_64 = 56;

    off_t phoff = 0;
    uint16_t phentsize = 0;
    uint16_t phnum = 0;
  auto rd = [&](size_t off, size_t size) -> uint64_t {
    if (off + size > header_cache_.size()) return 0;
    return decode(header_cache_.data() + off, size, info_.ei_data);
  };

  if (is64) {
    phoff = static_cast<off_t>(rd(PHOFF_64, 8));
    phentsize = static_cast<uint16_t>(rd(PHENTSIZE_64, 2));
    phnum = static_cast<uint16_t>(rd(PHNUM_64, 2));
  } else {
    phoff = static_cast<off_t>(rd(PHOFF_32, 4));
    phentsize = static_cast<uint16_t>(rd(PHENTSIZE_32, 2));
    phnum = static_cast<uint16_t>(rd(PHNUM_32, 2));
  }

    const size_t maxScan = 128; // Cap scans for safety.
    size_t nScan = phnum < maxScan ? phnum : maxScan;

    for (size_t i = 0; i < nScan; ++i) {
        if (::lseek(fd_.get(), phoff + i * phentsize, SEEK_SET) == -1) break;
  std::array<unsigned char, 4> type{};
  if (!util::read_exact(fd_.get(), type.data(), type.size())) break;
  uint32_t p_type = static_cast<uint32_t>(decode(
      type.data(), type.size(), info_.ei_data));

        constexpr uint32_t PT_INTERP = 3;
        if (p_type == PT_INTERP) {
            is_static_ = false;
            return;
        }
    }

  // Heuristic: if we never saw PT_INTERP and PH table looked sane, assume static.
  is_static_ = (phoff > 0 && phentsize > 0 && phnum > 0);
}


// --- Public API Functions ---

std::string detectHostArch() {
  struct utsname u{}; if (::uname(&u) != 0) return {};
  return normalize(u.machine);
}

std::string qemuUserEmulatorForArch(const std::string& arch) {
  if (arch == "x86_64") return "qemu-x86_64";
  if (arch == "x86") return "qemu-i386";
  if (arch == "arm64") return "qemu-aarch64";
  if (arch == "arm64_be") return "qemu-aarch64_be";
  if (arch == "arm") return "qemu-arm";
  if (arch == "armeb") return "qemu-armeb";
  if (arch == "ppc64") return "qemu-ppc64";
  if (arch == "ppc64le") return "qemu-ppc64le";
  if (arch == "riscv32") return "qemu-riscv32";
  if (arch == "riscv64") return "qemu-riscv64";
  return {};
}

std::string detectElfArchAt(const std::string& root, const std::string& probeTarget) {
    return readElfInfoAtRoot(root, probeTarget).archName;
}

ElfInfo readElfInfoAtRoot(const std::string& root,
                          const std::string& probeTarget) {
  if (root.empty() || probeTarget.empty()) return {};
  util::Rootfs rootfs(root);
  UniqueFd file = rootfs.resolvedFile(probeTarget);
  if (!file) return {};
  ElfParser parser(file.get());
  return parser.getInfo();
}

util::Result<ElfInfo, util::Error> readElfInfoAtPathEx(const std::string& absPath) {
  ElfParser parser(absPath);
  ElfInfo info = parser.getInfo();
  if (!info.valid) {
    // Determine rough cause: not ELF vs IO
    struct stat st{};
    if (::stat(absPath.c_str(), &st) != 0) {
      return util::Result<ElfInfo, util::Error>::err(util::make_error(util::LibErr::Io, errno, std::string("stat failed: ") + absPath));
    }
    return util::Result<ElfInfo, util::Error>::err(util::make_error(util::LibErr::Invalid, 0, std::string("not an ELF: ") + absPath));
  }
  return util::Result<ElfInfo, util::Error>::ok(std::move(info));
}

ElfInfo readElfInfoAtPath(const std::string& absPath) {
  auto r = readElfInfoAtPathEx(absPath);
  return r.ok() ? r.value() : ElfInfo{};
}

bool isStaticElf(const std::string& absPath) {
    ElfParser parser(absPath);
    return parser.isStatic();
}

const char* elfClassToString(unsigned char ei_class) {
  switch (ei_class) {
    case 1: return "ELFCLASS32";
    case 2: return "ELFCLASS64";
    default: return "UNKNOWN";
  }
}

const char* elfDataToString(unsigned char ei_data) {
  switch (ei_data) {
    case 1: return "ELFDATA2LSB";
    case 2: return "ELFDATA2MSB";
    default: return "UNKNOWN";
  }
}

const char* elfOsAbiToString(unsigned char ei_osabi) {
  switch (ei_osabi) {
    case 0: return "ELFOSABI_SYSV";
    case 3: return "ELFOSABI_LINUX";
    case 9: return "ELFOSABI_FREEBSD";
    default: return "ELFOSABI_OTHER";
  }
}
