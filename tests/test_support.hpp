#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#ifndef UNROOT_TEST_BUILD_DIR
#define UNROOT_TEST_BUILD_DIR "build"
#endif

namespace test {

class TempDirectory {
  static std::filesystem::path create() {
    const auto build = std::filesystem::absolute(UNROOT_TEST_BUILD_DIR);
    std::filesystem::create_directories(build);
    std::string pattern = (build / "test-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::system_error(errno, std::generic_category(), "mkdtemp");
    return pattern;
  }

 public:
  const std::filesystem::path root = create();
  TempDirectory() = default;
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (error) std::cerr << "fixture cleanup failed: " << root << ": " << error.message() << '\n';
  }
};

class ScopedEnvironment {
  std::string name_;
  std::optional<std::string> previous_;

 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    if (const char* previous = ::getenv(name)) previous_ = previous;
    const int result = value ? ::setenv(name, value, 1) : ::unsetenv(name);
    if (result != 0)
      throw std::system_error(errno, std::generic_category(), "set environment");
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
  ~ScopedEnvironment() {
    if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
    else ::unsetenv(name_.c_str());
  }
};

}  // namespace test
