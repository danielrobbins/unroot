#pragma once

#include <string>

#include "nlohmann/json.hpp"

enum class CapabilityStatus { Available, Unavailable, Unknown };

const char* capabilityStatusName(CapabilityStatus status);

struct Capability {
  CapabilityStatus status = CapabilityStatus::Unknown;
  std::string detail;

  bool available() const { return status == CapabilityStatus::Available; }
  nlohmann::json toJson() const;
};

class HostCaps {
 public:
  HostCaps();

  const std::string& kernelRelease() const { return kernelRelease_; }
  const std::string& machine() const { return machine_; }
  const Capability& userNamespaces() const { return userNamespaces_; }
  const Capability& mountNamespaces() const { return mountNamespaces_; }
  const Capability& binfmtMisc() const { return binfmtMisc_; }
  const Capability& setgroups() const { return setgroups_; }
  const Capability& newuidmap() const { return newuidmap_; }
  const Capability& newgidmap() const { return newgidmap_; }
  const Capability& hostHelper() const { return hostHelper_; }
  const Capability& archiveEngine() const { return archiveEngine_; }

  nlohmann::json toJson() const;

 private:
  std::string kernelRelease_;
  std::string machine_;
  Capability userNamespaces_;
  Capability mountNamespaces_;
  Capability binfmtMisc_;
  Capability setgroups_;
  Capability newuidmap_;
  Capability newgidmap_;
  Capability hostHelper_;
  Capability archiveEngine_;
};

const HostCaps& getGlobalHostCaps();
