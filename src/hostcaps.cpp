// hostcaps.cpp
#include "hostcaps.hpp"
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include "util/file.hpp"

namespace {
struct ProbeResult { bool ok=false; };
static ProbeResult probeUnshareRaw(unsigned long flag){ ProbeResult pr; pid_t p=::fork(); if(p<0) return pr; if(p==0){ int rc=(::unshare(flag)==0)?0:1; _exit(rc);} int st=0; if(::waitpid(p,&st,0)<0) return pr; if(WIFEXITED(st) && WEXITSTATUS(st)==0) pr.ok=true; return pr; }
static ProbeResult probeNs(unsigned long flag, bool userSupported){ if(flag==CLONE_NEWUSER) return probeUnshareRaw(flag); ProbeResult pr=probeUnshareRaw(flag); if(pr.ok) return pr; if(userSupported){ ProbeResult comb=probeUnshareRaw(CLONE_NEWUSER|flag); if(comb.ok) return comb; } return pr; }
static bool pathExists(const char* p){ struct stat st{}; return ::stat(p,&st)==0; }
static bool pathWritable(const char* p){ int fd=::open(p,O_WRONLY|O_CLOEXEC); if(fd<0) return false; ::close(fd); return true; }
static bool probeOverlayfs() {
    char workspace[] = "/tmp/.unroot-ovl-test-XXXXXX";
    char* base = ::mkdtemp(workspace);
    if (!base) return false;
    std::string lower = std::string(base) + "/lower";
    std::string upper = std::string(base) + "/upper";
    std::string work = std::string(base) + "/work";
    std::string mountpoint = std::string(base) + "/mnt";
    bool prepared = ::mkdir(lower.c_str(), 0700) == 0 &&
                    ::mkdir(upper.c_str(), 0700) == 0 &&
                    ::mkdir(work.c_str(), 0700) == 0 &&
                    ::mkdir(mountpoint.c_str(), 0700) == 0;
    std::string options = "lowerdir=" + lower + ",upperdir=" + upper +
                          ",workdir=" + work;
    bool mounted = prepared &&
                   ::mount("overlay", mountpoint.c_str(), "overlay", 0,
                           options.c_str()) == 0;
    if (mounted) (void)::umount(mountpoint.c_str());
    (void)::rmdir(mountpoint.c_str());
    (void)::rmdir(work.c_str());
    (void)::rmdir(upper.c_str());
    (void)::rmdir(lower.c_str());
    (void)::rmdir(base);
    return mounted;
}
}

void HostCaps::probe() {
    m_probedEpoch = static_cast<long long>(::time(nullptr));
    struct utsname un{}; if(::uname(&un)==0){ m_kernelRelease=un.release; m_kernelNode=un.nodename; }
    auto user = probeNs(CLONE_NEWUSER,false); m_userNs = user.ok; bool userSupported = m_userNs;
    m_pidNs = probeNs(CLONE_NEWPID,userSupported).ok;
    m_mntNs = probeNs(CLONE_NEWNS,userSupported).ok;
#ifdef CLONE_NEWUTS
    m_utsNs = probeNs(CLONE_NEWUTS,userSupported).ok;
#endif
#ifdef CLONE_NEWIPC
    m_ipcNs = probeNs(CLONE_NEWIPC,userSupported).ok;
#endif
#ifdef CLONE_NEWNET
    m_netNs = probeNs(CLONE_NEWNET,userSupported).ok;
#endif
#ifdef CLONE_NEWCGROUP
    m_cgroupNs = probeNs(CLONE_NEWCGROUP,userSupported).ok;
#endif
    m_binfmtFs = pathExists("/proc/sys/fs/binfmt_misc");
    m_binfmtRegWritable = pathWritable("/proc/sys/fs/binfmt_misc/register");
    m_setgroupsFile = pathExists("/proc/self/setgroups");
    m_setgroupsWritable = m_setgroupsFile && pathWritable("/proc/self/setgroups");
    // helpers
    m_newuidmap = (::access("/usr/bin/newuidmap", X_OK)==0);
    m_newgidmap = (::access("/usr/bin/newgidmap", X_OK)==0);
    // seccomp
    std::string status = util::readSmallFile("/proc/self/status"); m_seccomp = (status.find("Seccomp:")!=std::string::npos);
    // cgroup v2
    struct stat st{}; if(::stat("/sys/fs/cgroup/cgroup.controllers",&st)==0) m_cgroupV2=true; else if(::stat("/sys/fs/cgroup/unified",&st)==0) m_cgroupV2=true;
    // overlayfs (best-effort diagnostic mount)
    m_overlayfs = probeOverlayfs();
    // shiftfs
    m_shiftfs = pathExists("/sys/module/shiftfs");
    // idmapped mounts
#ifdef SYS_mount_setattr
    errno=0; long rc = ::syscall(SYS_mount_setattr,-1,"",0U,nullptr,0U); m_idmappedMounts=(rc==-1 && errno==EBADF);
#endif
}

HostCaps& getGlobalHostCaps() {
    static HostCaps caps; static bool probed=false; if(!probed){ caps.probe(); probed=true; } return caps;
}

void reprobeGlobalHostCaps() { HostCaps& c = getGlobalHostCaps(); c.probe(); }

nlohmann::json HostCaps::toJson(bool includeTimestamp) const {
    nlohmann::json hc = nlohmann::json::object();
    hc["kernelRelease"] = m_kernelRelease;
    hc["kernelNode"] = m_kernelNode;
    hc["namespaces"] = {
        {"user", m_userNs}, {"pid", m_pidNs}, {"mount", m_mntNs}, {"uts", m_utsNs}, {"ipc", m_ipcNs}, {"net", m_netNs}, {"cgroup", m_cgroupNs}
    };
    hc["binfmt"] = {
        {"hostMounted", m_binfmtFs}, {"hostRegisterWritable", m_binfmtRegWritable}
    };
    hc["setgroups"] = { {"present", m_setgroupsFile}, {"writable", m_setgroupsWritable} };
    hc["helpers"] = { {"newuidmap", m_newuidmap}, {"newgidmap", m_newgidmap} };
    hc["seccomp"] = m_seccomp;
    hc["cgroupV2"] = m_cgroupV2;
    hc["overlayfs"] = m_overlayfs;
    hc["shiftfs"] = m_shiftfs;
    hc["idmappedMounts"] = m_idmappedMounts;
    if (includeTimestamp) hc["probedEpoch"] = m_probedEpoch;
    return hc;
}
