// arch.hpp — simple ELF/host arch detection and mapping for unroot
#pragma once
#include <string>
#include "util/result.hpp"
#include "util/errors.hpp"

struct ElfInfo {
  bool valid = false;
  unsigned char ei_class = 0;
  unsigned char ei_data = 0;
  unsigned char ei_osabi = 0;
  unsigned short e_machine = 0;
  std::string archName;
};

// Detect host architecture (uname -m simplified to normalized names)
std::string detectHostArch();

// Return the conventional QEMU user-mode emulator name for a normalized
// target architecture, or an empty string when no default is known.
std::string qemuUserEmulatorForArch(const std::string& arch);

// Detect target arch by reading ELF header of a file under rootfs.
// Returns empty string if not ELF or unknown.
std::string detectElfArchAt(const std::string& rootfs, const std::string& pathInRootfs);

// Read ELF metadata through rootfs-contained symlink resolution.
ElfInfo readElfInfoAtRoot(const std::string& rootfs,
                          const std::string& pathInRootfs);

// Read basic ELF header info from an absolute path. Returns ElfInfo with valid=false if not ELF.
ElfInfo readElfInfoAtPath(const std::string& absPath);

// Result-returning variant with error details (non-breaking; internal use for migration)
util::Result<ElfInfo, util::Error> readElfInfoAtPathEx(const std::string& absPath);

// Human-friendly strings for ELF header fields
const char* elfClassToString(unsigned char ei_class);
const char* elfDataToString(unsigned char ei_data);
const char* elfOsAbiToString(unsigned char ei_osabi);

// Return true if ELF appears static (no PT_INTERP).
bool isStaticElf(const std::string& absPath);
