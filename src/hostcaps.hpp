// hostcaps.hpp -- host capability probing (namespaces, binfmt, helpers, kernel features)
#pragma once
#include <string>
#include "nlohmann/json.hpp"

class HostCaps {
public:
    void probe();
    // Accessors
    bool hasUserNamespaces() const { return m_userNs; }
    bool hasPidNamespaces() const { return m_pidNs; }
    bool hasMountNamespaces() const { return m_mntNs; }
    bool hasUtsNamespaces() const { return m_utsNs; }
    bool hasIpcNamespaces() const { return m_ipcNs; }
    bool hasNetNamespaces() const { return m_netNs; }
    bool hasCgroupNamespaces() const { return m_cgroupNs; }
    bool hasBinfmtFs() const { return m_binfmtFs; }
    bool isBinfmtRegisterWritable() const { return m_binfmtRegWritable; }
    bool hasSetgroupsFile() const { return m_setgroupsFile; }
    bool isSetgroupsWritable() const { return m_setgroupsWritable; }
    bool hasSeccomp() const { return m_seccomp; }
    bool hasCgroupV2() const { return m_cgroupV2; }
    bool hasOverlayfs() const { return m_overlayfs; }
    bool hasShiftfs() const { return m_shiftfs; }
    bool hasIdmappedMounts() const { return m_idmappedMounts; }
    bool hasNewUIDMapHelper() const { return m_newuidmap; }
    bool hasNewGIDMapHelper() const { return m_newgidmap; }

    const std::string& kernelRelease() const { return m_kernelRelease; }
    const std::string& kernelNode() const { return m_kernelNode; }

    // Serialize to JSON snapshot (includes probedEpoch by default)
    nlohmann::json toJson(bool includeTimestamp = true) const;

private:
    // Cached values
    bool m_userNs=false, m_pidNs=false, m_mntNs=false, m_utsNs=false, m_ipcNs=false, m_netNs=false, m_cgroupNs=false;
    bool m_binfmtFs=false, m_binfmtRegWritable=false;
    bool m_setgroupsFile=false, m_setgroupsWritable=false;
    bool m_seccomp=false, m_cgroupV2=false, m_overlayfs=false, m_shiftfs=false, m_idmappedMounts=false;
    bool m_newuidmap=false, m_newgidmap=false;
    std::string m_kernelRelease; std::string m_kernelNode; long long m_probedEpoch = 0;
};

// Global accessor (lazy probe on first use)
HostCaps& getGlobalHostCaps();
// Force a re-probe (non-idempotent; only for tests/diagnostics after environment changes)
void reprobeGlobalHostCaps();
