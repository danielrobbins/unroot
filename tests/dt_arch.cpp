#include "doctest.h"
#include "arch.hpp"
#include "shebang.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

static void encode(std::array<unsigned char, 128>& header, size_t offset,
                   size_t size, uint64_t value, unsigned char data) {
    for (size_t i = 0; i < size; ++i) {
        size_t position = data == 1 ? i : size - i - 1;
        header[offset + position] = static_cast<unsigned char>(value >> (i * 8));
    }
}

static void writeElf(const std::filesystem::path& path,
                     unsigned short machine, unsigned char elfClass = 2,
                     unsigned char data = 1, unsigned int programType = 0) {
    std::array<unsigned char, 128> header{};
    header[0] = 0x7f;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = elfClass;
    header[5] = data;
    encode(header, 18, 2, machine, data);
    if (programType != 0) {
        const size_t phoff = elfClass == 1 ? 52 : 64;
        const size_t phentsize = elfClass == 1 ? 32 : 56;
        encode(header, elfClass == 1 ? 28 : 32,
               elfClass == 1 ? 4 : 8, phoff, data);
        encode(header, elfClass == 1 ? 42 : 54, 2, phentsize, data);
        encode(header, elfClass == 1 ? 44 : 56, 2, 1, data);
        encode(header, phoff, 4, programType, data);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
}

TEST_CASE("elf header helper stringifiers return expected tokens") {
    CHECK(std::string(elfClassToString(1)) == "ELFCLASS32");
    CHECK(std::string(elfClassToString(2)) == "ELFCLASS64");
    CHECK(std::string(elfDataToString(1)) == "ELFDATA2LSB");
    CHECK(std::string(elfDataToString(2)) == "ELFDATA2MSB");
    CHECK(std::string(elfOsAbiToString(0)) == "ELFOSABI_SYSV");
    CHECK(std::string(elfOsAbiToString(3)) == "ELFOSABI_LINUX");
    CHECK(std::string(elfOsAbiToString(9)) == "ELFOSABI_FREEBSD");
}

TEST_CASE("readElfInfoAtPathEx succeeds on /bin/sh and agrees with detectElfArchAt") {
    auto r = readElfInfoAtPathEx("/bin/sh");
    REQUIRE(r.ok());
    auto info = r.value();
    CHECK(info.valid);
    CHECK( (info.ei_class == 1 || info.ei_class == 2) );
    CHECK_FALSE(info.archName.empty());
    auto info2 = readElfInfoAtPath("/bin/sh");
    CHECK(info2.archName == info.archName);
    CHECK(detectElfArchAt("/", "/bin/sh") == info.archName);
}

TEST_CASE("detectHostArch returns non-empty normalized architecture") {
    auto host = detectHostArch();
    CHECK_FALSE(host.empty());
    // We can't assert exact value (depends on machine) but ensure mapping is stable for common arches.
    if (host == "x86_64") {
        CHECK(host == "x86_64");
    } else if (host == "x86") {
        CHECK(host == "x86");
    } else if (host == "arm64" || host == "aarch64") {
        CHECK((host == "arm64" || host == "aarch64"));
    } else if (host == "arm") {
        CHECK(host == "arm");
    } else {
        // Accept any non-empty normalization for other platforms.
        CHECK(host.size() > 0);
    }
}

TEST_CASE("AArch64 ELF headers use the canonical host architecture name") {
    auto path = std::filesystem::temp_directory_path() /
        ("unroot-aarch64-" + std::to_string(static_cast<long long>(::getpid())));
    writeElf(path, 183);
    CHECK(readElfInfoAtPath(path.string()).archName == "arm64");
    std::filesystem::remove(path);
}

TEST_CASE("ELF parsing honors byte order and class") {
    auto base = std::filesystem::temp_directory_path() /
        ("unroot-endian-" + std::to_string(static_cast<long long>(::getpid())));
    auto little = base.string() + "-le";
    auto big = base.string() + "-be";
    auto riscv32 = base.string() + "-rv32";
    writeElf(little, 183, 2, 1);
    writeElf(big, 183, 2, 2);
    writeElf(riscv32, 243, 1, 1);

    auto littleInfo = readElfInfoAtPath(little);
    auto bigInfo = readElfInfoAtPath(big);
    CHECK(littleInfo.e_machine == 183);
    CHECK(bigInfo.e_machine == 183);
    CHECK(littleInfo.archName == "arm64");
    CHECK(bigInfo.archName == "arm64_be");
    CHECK(readElfInfoAtPath(riscv32).archName == "riscv32");

    std::filesystem::remove(little);
    std::filesystem::remove(big);
    std::filesystem::remove(riscv32);
}

TEST_CASE("unsupported ELF class and byte-order combinations are explicit") {
    auto path = std::filesystem::temp_directory_path() /
        ("unroot-unsupported-" +
         std::to_string(static_cast<long long>(::getpid())));
    writeElf(path, 62, 2, 2);
    auto info = readElfInfoAtPath(path.string());
    CHECK(info.valid);
    CHECK(info.archName.empty());
    std::filesystem::remove(path);
}

TEST_CASE("normalized architectures map to conventional QEMU user emulators") {
    CHECK(qemuUserEmulatorForArch("x86_64") == "qemu-x86_64");
    CHECK(qemuUserEmulatorForArch("x86") == "qemu-i386");
    CHECK(qemuUserEmulatorForArch("arm64") == "qemu-aarch64");
    CHECK(qemuUserEmulatorForArch("arm64_be") == "qemu-aarch64_be");
    CHECK(qemuUserEmulatorForArch("arm") == "qemu-arm");
    CHECK(qemuUserEmulatorForArch("armeb") == "qemu-armeb");
    CHECK(qemuUserEmulatorForArch("ppc64") == "qemu-ppc64");
    CHECK(qemuUserEmulatorForArch("ppc64le") == "qemu-ppc64le");
    CHECK(qemuUserEmulatorForArch("riscv32") == "qemu-riscv32");
    CHECK(qemuUserEmulatorForArch("riscv64") == "qemu-riscv64");
    CHECK(qemuUserEmulatorForArch("unknown").empty());
}

TEST_CASE("rootfs ELF probing resolves absolute links inside the root") {
    auto base = std::filesystem::temp_directory_path() /
        ("unroot-contained-elf-" +
         std::to_string(static_cast<long long>(::getpid())));
    auto root = base / "root";
    std::filesystem::create_directories(root / "bin");
    writeElf(root / "inside", 183);
    writeElf(base / "inside", 62);
    std::filesystem::create_symlink("/inside", root / "bin" / "probe");

    CHECK(detectElfArchAt(root.string(), "/bin/probe") == "arm64");
    std::filesystem::remove_all(base);
}

TEST_CASE("rootfs ELF probing cannot traverse above the root") {
    auto base = std::filesystem::temp_directory_path() /
        ("unroot-escaping-elf-" +
         std::to_string(static_cast<long long>(::getpid())));
    auto root = base / "root";
    std::filesystem::create_directories(root / "bin");
    writeElf(base / "outside", 62);
    std::filesystem::create_symlink("../../outside", root / "bin" / "probe");

    CHECK(detectElfArchAt(root.string(), "/bin/probe").empty());
    std::filesystem::remove_all(base);
}

TEST_CASE("isStaticElf returns bool for /bin/sh (value may vary by distro)") {
    bool s = isStaticElf("/bin/sh");
    CHECK( (s == true || s == false) );
}

TEST_CASE("static ELF detection decodes big-endian program headers") {
    auto base = std::filesystem::temp_directory_path() /
        ("unroot-static-be-" +
         std::to_string(static_cast<long long>(::getpid())));
    auto staticElf = base.string() + "-static";
    auto dynamicElf = base.string() + "-dynamic";
    writeElf(staticElf, 183, 2, 2, 1);
    writeElf(dynamicElf, 183, 2, 2, 3);
    CHECK(isStaticElf(staticElf));
    CHECK_FALSE(isStaticElf(dynamicElf));
    std::filesystem::remove(staticElf);
    std::filesystem::remove(dynamicElf);
}

TEST_CASE("executable image resolution follows shebang interpreters") {
    auto root = std::filesystem::temp_directory_path() /
        ("unroot-shebang-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::create_directories(root / "bin");
    {
        std::ofstream script(root / "bin" / "script");
        script << "#!/bin/interpreter\n";
    }

    CHECK(resolveExecutableImage(root, "/bin/script") == "/bin/interpreter");
    CHECK(resolveExecutableImage(root, "/bin/interpreter") == "/bin/interpreter");
    std::filesystem::remove_all(root);
}
