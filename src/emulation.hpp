#pragma once

#include <string>
#include <utility>

#include "arch.hpp"
#include "linuxns.hpp"

namespace emulation {

enum class Policy { Auto, Never };

const char* policyName(Policy policy);

struct Settings {
  bool hostVisible = false;
  bool native = false;
  std::string root;
  std::string executable;
  std::string hostArch;
  std::string targetArch;
  Policy policy = Policy::Auto;
  std::string qemu;
  std::string cpu;
};

class Manager {
 public:
  explicit Manager(Settings settings) : settings_(std::move(settings)) {}

 EmuPlan prepare();

 private:
  Settings settings_;
};

}  // namespace emulation
