// linuxns.cpp - user, mount, and PID namespaces; ID maps; chroot; exec
#include "linuxns.hpp"
#include "diagnostics.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <cstring>
#include <sys/wait.h>
#include <libgen.h>
#include <cstdlib>
#include <unordered_map>
#include <cctype>
#include <cstdint>
#include <sys/utsname.h>
#include <dirent.h>
#include <limits.h>
#include "util/fd.hpp"
#include "util/proc.hpp"
#include "util/io.hpp"
#include "util/log.hpp"
#include "util/rootfs.hpp"

namespace {

// Logging and step tracking toggles (env-driven)
[[maybe_unused]] static bool envFlag(const char* name) {
  const char* v = ::getenv(name);
  return v && (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T');
}
// Verbosity is evaluated dynamically (don't snapshot at static init; --debug sets
// env vars later in main). 0=off,1=essential,2=all.
static int currentVerboseLevel(){ const char* v = ::getenv("UNROOT_VERBOSE"); return (v && *v) ? std::atoi(v) : 0; }
static bool currentJsonLog(){ const char* v = ::getenv("UNROOT_LOG"); return v && std::strcmp(v, "json") == 0; }
// Overrides captured inside namespace before we sanitize env (=-1 means unused)
static int gNsVerboseOverride = -1; // 0/1/2 when set
static int gNsJsonOverride = -1;    // 0/1 when set
[[maybe_unused]] static bool gStrict = [](){ const char* v = ::getenv("UNROOT_STRICT");
  if (!v || !*v) return true; // strict by default
  if (*v=='0'||*v=='n'||*v=='N'||*v=='f'||*v=='F') return false;
  return (*v=='1'||*v=='y'||*v=='Y'||*v=='t'||*v=='T'); }() ;
struct Step { const char* name; bool ok; int err; bool essential; const char* note; };
static void logStep(const Step& s) {
  int lvl = (gNsVerboseOverride >= 0) ? gNsVerboseOverride : currentVerboseLevel();
  // Verbosity: 0=off, 1=essential only, 2=all
  if (lvl < 1) return;
  if (!s.essential && lvl < 2) return;
  // If a sink is installed, use it; otherwise, fall back to existing stderr behavior.
  util::StepView sv{ s.name, s.ok, s.err, s.note };
  if (util::globalLogSink()) {
    util::emitLog(s.ok ? util::LogLevel::Info : util::LogLevel::Error, sv);
    return;
  }
  bool json = (gNsJsonOverride >= 0) ? (gNsJsonOverride == 1) : currentJsonLog();
  if (json) {
    std::fprintf(stderr,
      "{\"step\":\"%s\",\"ok\":%s,\"err\":%d,\"note\":\"%s\"}\n",
      s.name,
      s.ok ? "true" : "false",
      s.err,
      s.note ? s.note : "");
  } else {
    std::fprintf(stderr,
      "[%s] ok=%s err=%d%s%s\n",
      s.name,
      s.ok ? "true" : "false",
      s.err,
      s.err ? " (" : "",
      s.err ? std::strerror(s.err) : "");
    if (s.note && *s.note) std::fprintf(stderr, "  note: %s\n", s.note);
  }
 }

static void recordStep(const char* name, bool ok, bool essential,
                       const char* note) {
  logStep({name, ok, ok ? 0 : errno, essential, note});
}

// Tiny mount wrapper: returns true on success, false on failure, preserving errno
static inline bool mount_ok(const char* src, const char* target, const char* fstype,
                            unsigned long flags, const void* data) {
  return ::mount(src, target, fstype, flags, data) == 0;
}

static bool isProcUsable() {
  struct stat st{};
  return ::stat("/proc/self", &st) == 0;
}

// Feature toggles parsed from UNROOT_FEATURES
class FeatureSet {
public:
  FeatureSet(bool hostVisible = false) {
  for (auto& k : known()) enabled_[k] = true; // start enabled
  // Default to minimal /dev handling: do not bind-mount the entire /dev
  enabled_["devbind"] = false;
  // Do not mount a tmpfs over /tmp by default (preserve host /tmp semantics)
  enabled_["tmp"] = false;
  // In single mode, avoid over-mounting visible host paths by default.
  // Users can still opt in via UNROOT_FEATURES=+run,+shm.
  if (hostVisible) {
    enabled_["run"] = false;
    enabled_["shm"] = false;
  }
  }
  static const std::vector<std::string>& known() {
  static std::vector<std::string> v{
  "devbind","resolvconf","hosts","proc",
  "devpts","shm","run","mtab"
  };
    return v;
  }
  void parseEnv() {
    const char* ev = ::getenv("UNROOT_FEATURES");
    if (!ev || !*ev) return; // keep defaults
    std::string s(ev);
    std::unordered_map<std::string,int> counts;
    size_t start = 0;
    while (start <= s.size()) {
      size_t comma = s.find(',', start);
      std::string tok = s.substr(start, (comma==std::string::npos? s.size(): comma) - start);
      // trim spaces
      size_t l = 0, r = tok.size();
      while (l < r && std::isspace(static_cast<unsigned char>(tok[l]))) ++l;
      while (r > l && std::isspace(static_cast<unsigned char>(tok[r-1]))) --r;
      tok = tok.substr(l, r-l);
      if (!tok.empty()) applyToken(tok, counts);
      if (comma == std::string::npos) break; else start = comma + 1;
    }
    for (auto& kv : counts) if (kv.second > 1)
      std::fprintf(stderr, "unroot: warn feature '%s' toggled multiple times\n", kv.first.c_str());
    enabled_["proc"] = true;
  }
  bool has(const char* name) const {
    auto it = enabled_.find(name);
    return it != enabled_.end() && it->second;
  }
  std::vector<std::string> names() const {
    std::vector<std::string> names;
    for (const auto& name : known()) if (has(name.c_str())) names.push_back(name);
    return names;
  }
private:
  void disableAll() { for (auto& kv : enabled_) kv.second = false; }
  void applyToken(const std::string& tok, std::unordered_map<std::string,int>& counts) {
    if (tok == "-*" ) { disableAll(); return; }
    bool en = true; size_t i = 0;
    if (tok[0] == '+') { en = true; i = 1; }
    else if (tok[0] == '-') { en = false; i = 1; }
    std::string name = tok.substr(i);
    if (name == "*") { for (auto& kv : enabled_) kv.second = en; return; }
    if (enabled_.count(name) == 0) return; // ignore unknown
    enabled_[name] = en;
    counts[name] += 1;
  }
  std::unordered_map<std::string,bool> enabled_;
};

struct NsOptions {
  bool bindDev = true;
  bool bindResolvConf = true;
  bool bindHosts = true;
  bool mountProc = true;
  bool mountDevpts = true;
  bool mountTmpfsShm = true;
  bool mountRun = true;
  // removed: mountTmp (no tmpfs over /tmp)
  bool linkMtab = true;
};

static NsOptions defaultOptionsAllTrue() {
  NsOptions o;
  o.bindDev = true;
  o.bindResolvConf = true;
  o.bindHosts = true;
  o.mountProc = true;
  o.mountDevpts = true;
  o.mountTmpfsShm = true;
  o.mountRun = true;
  // removed: tmpfs /tmp
  o.linkMtab = true;
  return o;
}

static NsOptions resolveOptionsFromEnv(bool hostVisible = false) {
  auto o = defaultOptionsAllTrue();
  FeatureSet fs(hostVisible);
  fs.parseEnv();
  o.bindDev = fs.has("devbind");
  o.bindResolvConf = fs.has("resolvconf");
  o.bindHosts = fs.has("hosts");
  // proc is always-on regardless of env toggles
  o.mountProc = true;
  o.mountDevpts = fs.has("devpts");
  o.mountTmpfsShm = fs.has("shm");
  o.mountRun = fs.has("run");
  // removed: tmp feature
  o.linkMtab = fs.has("mtab");
  return o;
}

static bool mountBind(const char* src, const std::string& dst, bool recursive,
                      const char* stepName, bool essential, const char* note) {
  unsigned long flags = MS_BIND | (recursive ? (unsigned long)MS_REC : 0UL);
  bool mounted = mount_ok(src, dst.c_str(), nullptr, flags, nullptr);
  recordStep(stepName, mounted, essential, note);
  return mounted;
}

static bool setMountReadonly(int target, bool recursive) {
  struct MountAttributes {
    std::uint64_t set;
    std::uint64_t clear;
    std::uint64_t propagation;
    std::uint64_t userNamespace;
  } attributes{1, 0, 0, 0};  // MOUNT_ATTR_RDONLY
#if defined(SYS_mount_setattr)
  constexpr long mountSetattr = SYS_mount_setattr;
#elif defined(__x86_64__) || defined(__aarch64__)
  constexpr long mountSetattr = 442;
#else
  errno = ENOSYS;
  return false;
#endif
  constexpr unsigned int atRecursive = 0x8000;
  unsigned int flags = AT_EMPTY_PATH | (recursive ? atRecursive : 0);
  return ::syscall(mountSetattr, target, "", flags, &attributes,
                   sizeof(attributes)) == 0;
}

static bool bindRootfsTarget(const util::Rootfs& root, const char* src,
                             const std::string& dst, bool directory,
                             bool recursive, bool readonly,
                             const char* stepName, bool essential = false,
                             const char* note = nullptr) {
  UniqueFd target = root.mountTarget(dst, directory);
  if (!target) {
    recordStep(stepName, false, essential, "cannot prepare safe bind target");
    return false;
  }
  if (!mountBind(src, util::Rootfs::fdPath(target.get()), recursive, stepName,
                 essential, note))
    return false;
  if (!readonly) return true;

  // Reopen after the bind so this descriptor refers to the mounted object,
  // rather than the underlying target hidden by it.
  UniqueFd mounted = root.mountTarget(dst, directory);
  std::string readonlyStep = std::string(stepName) + ":ro";
  if (!mounted) {
    recordStep(readonlyStep.c_str(), false, essential,
               "cannot reopen readonly bind target");
    return false;
  }
  std::string mountedPath = util::Rootfs::fdPath(mounted.get());
  bool protectedMount = setMountReadonly(mounted.get(), recursive);
  recordStep(readonlyStep.c_str(), protectedMount, essential,
             "required readonly remount");
  if (!protectedMount) (void)::umount2(mountedPath.c_str(), MNT_DETACH);
  return protectedMount;
}

static bool setupPreChrootBinds(const util::Rootfs& root,
                                const NsOptions& opt, const EmuPlan* emu,
                                const std::vector<BindMap>* maps) {
  // Optional emulator bind (for cross-arch without binfmt): host file -> /tmp/unroot/<name> in rootfs
  if (emu && !emu->source.empty() && !emu->target.empty()) {
    std::string note = std::string("static emulator: src=") + emu->source +
                       " dst=" + emu->target;
    if (!bindRootfsTarget(root, emu->source.c_str(), emu->target, false, false,
                          true, "bind:emu", true, note.c_str())) return false;
  }
  // User-requested map binds (enforced): abort on failure
  if (maps) {
    for (const auto& m : *maps) {
      if (m.dst.empty() || m.dst.front() != '/' || m.dst == "/") {
        recordStep("bind:map:invalid", false, true, "dst invalid");
        return false;
      }
      struct stat st{};
      if (::stat(m.src.c_str(), &st) != 0) {
        recordStep("bind:map:source", false, true, "source unavailable");
        return false;
      }
      bool isDir = S_ISDIR(st.st_mode);
      bool mounted = bindRootfsTarget(
          root, m.src.c_str(), m.dst, isDir, isDir, m.readonly,
          m.readonly ? (isDir ? "bind:map(dir,ro)" : "bind:map(file,ro)")
                     : (isDir ? "bind:map(dir,rw)" : "bind:map(file,rw)"),
          true, nullptr);
      if (!mounted) return false;
    }
  }
  // Only host binds pre-chroot
  if (opt.bindDev) {
    if (!bindRootfsTarget(root, "/dev", "/dev", true, true, false,
                          "bind:/dev", true, "essential device nodes"))
      return false;
  } else {
    // Minimal /dev: bind only a few essential character devices
    if (!bindRootfsTarget(root, "/dev/null", "/dev/null", false, false,
                          false, "bind:/dev/null", true, "minimal dev"))
      return false;
    if (!bindRootfsTarget(root, "/dev/zero", "/dev/zero", false, false,
                          false, "bind:/dev/zero", true, "minimal dev"))
      return false;
    for (const char* device : {"full", "tty", "console", "random", "urandom", "ptmx"}) {
      std::string path = std::string("/dev/") + device;
      std::string step = std::string("bind:") + path;
      (void)bindRootfsTarget(root, path.c_str(), path, false, false, false,
                             step.c_str(), false, "minimal dev");
    }
  }
  // Leave /sys alone by default; do not prebind /proc
  // Host network config files
  if (opt.bindResolvConf)
    (void)bindRootfsTarget(root, "/etc/resolv.conf", "/etc/resolv.conf", false,
                           false, true, "bind:/etc/resolv.conf", false,
                           "non-fatal network config");
  if (opt.bindHosts)
    (void)bindRootfsTarget(root, "/etc/hosts", "/etc/hosts", false, false,
                           true, "bind:/etc/hosts", false,
                           "non-fatal network config");
  return true;
}

static bool prepareMountDirectory(const char* path, bool hostVisible) {
  if (!hostVisible) return ensureDirAll(path);
  struct stat info{};
  return ::stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static void setupPostChrootMounts(const NsOptions& opt, bool hostVisible) {
  // proc
  if (opt.mountProc) {
    bool ok = prepareMountDirectory("/proc", hostVisible) &&
              mount_ok("proc", "/proc", "proc", 0, nullptr);
    recordStep("mount:proc", ok, true, "mounted procfs");
    if (!ok) {
      bool usable = isProcUsable();
      recordStep("proc:usable", usable, true, "strict requires /proc to be usable");
      if (gStrict && !usable) _exit(110);
    }
    // Always report the kernel in verbose logs.
    struct utsname un{}; if (::uname(&un) == 0) {
      recordStep("kernel", true, false, un.release);
    } else {
      recordStep("kernel", false, false, "uname failed");
    }
  }
  // devpts for pty support
  if (opt.mountDevpts) {
    bool prepared = prepareMountDirectory("/dev/pts", hostVisible);
    bool ok = prepared &&
              mount_ok("devpts", "/dev/pts", "devpts", 0,
                       "newinstance,ptmxmode=0666,mode=0620,gid=5");
    recordStep("mount:devpts(new)", ok, false, "best-effort newinstance");
    if (prepared && !ok && (errno == EINVAL || errno == EPERM)) {
      ok = mount_ok("devpts", "/dev/pts", "devpts", 0,
                    "ptmxmode=0666,mode=0620");
      recordStep("mount:devpts(std)", ok, false, "fallback options");
    }
    if (prepared && !ok) {
      ok = mount_ok("devpts", "/dev/pts", "devpts", 0, nullptr);
      recordStep("mount:devpts(none)", ok, false, "last resort");
    }
  }
  // Ensure /dev/ptmx points to pts/ptmx
  if (opt.mountDevpts && !hostVisible) {
    (void)::unlink("/dev/ptmx");
    bool ok = (::symlink("/dev/pts/ptmx", "/dev/ptmx") == 0);
    recordStep("link:/dev/ptmx", ok, false,
               "non-fatal: node may already exist");
  }
  // tmpfs for shm, run, tmp
  if (opt.mountTmpfsShm) {
    bool ok = prepareMountDirectory("/dev/shm", hostVisible) &&
              mount_ok("tmpfs", "/dev/shm", "tmpfs",
                       MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=1777,size=64M");
    recordStep("mount:tmpfs:/dev/shm", ok, false,
               "non-fatal cache/tmp space");
  }
  if (opt.mountRun) {
    bool ok = prepareMountDirectory("/run", hostVisible) &&
              mount_ok("tmpfs", "/run", "tmpfs",
                       MS_NOSUID | MS_NODEV | MS_NOEXEC, "mode=0755,size=16M");
    recordStep("mount:tmpfs:/run", ok, false, "non-fatal runtime dir");
  }
  // removed: tmpfs /tmp mount
  // Conventional /dev links used by shells and build tools.
  if (!hostVisible) {
    for (const auto& link : {
             std::pair{"/dev/fd", "/proc/self/fd"},
             std::pair{"/dev/stdin", "/proc/self/fd/0"},
             std::pair{"/dev/stdout", "/proc/self/fd/1"},
             std::pair{"/dev/stderr", "/proc/self/fd/2"},
         }) {
      struct stat st{};
      if (::lstat(link.first, &st) != 0) {
        bool ok = (::symlink(link.second, link.first) == 0);
        std::string step = std::string("link:") + link.first;
        recordStep(step.c_str(), ok, false, "minimal dev compatibility link");
      }
    }
  }
  // Helpful symlink under /etc.
  // Only create /etc/mtab symlink if it doesn't already exist.
  if (opt.linkMtab && !hostVisible) {
    struct stat st{};
    if (::lstat("/etc/mtab", &st) != 0) {
      ensureDirAll("/etc");
      bool ok = (::symlink("/proc/self/mounts", "/etc/mtab") == 0);
      recordStep("link:/etc/mtab", ok, false, "non-fatal compatibility link");
    }
  }
}

static bool setupPrivateBinfmt(const EmuPlan* emu) {
  if (!emu) return true;
  if (emu->registration.empty()) {
    recordStep("binfmt:register", false, true,
               "missing emulation registration");
    return false;
  }

  constexpr const char* root = "/proc/sys/fs/binfmt_misc";
  ensureDirAll(root);
  if (!mount_ok("binfmt_misc", root, "binfmt_misc", 0, nullptr)) {
    recordStep("binfmt:mount", false, true,
               "private binfmt_misc unavailable (requires Linux 6.7+ and runtime permission)");
    return false;
  }
  recordStep("binfmt:mount", true, true,
             "private user-namespace instance");

  UniqueFd fd(::open("/proc/sys/fs/binfmt_misc/register",
                     O_WRONLY | O_CLOEXEC));
  std::string registration = emu->registration + "\n";
  bool registered = fd &&
                    util::write_all(fd.get(), registration.data(),
                                    registration.size());
  recordStep("binfmt:register", registered, true,
             "private foreign-architecture handler");
  return registered;
}

} // namespace

// Exported helper: resolve enabled feature names from env (no dependency on internal types)
std::vector<std::string> resolveFeatureNamesFromEnv() {
  FeatureSet features;
  features.parseEnv();
  return features.names();
}

namespace {

struct SetupResult {
    int status = 0;
    int error = 0;
};

[[noreturn]] void reportSetupFailure(int output, int status);

// Common namespace setup for native and foreign-architecture target execution.
SetupResult setupNamespaceEnvironment(const std::string& rootfs, const EmuPlan* emu,
                                      const std::vector<BindMap>* maps,
                                      const NsEnvVars* envVars,
                                      const std::string* cwdInRoot) {
    // Set up mount namespace
    if (!mount_ok(nullptr, "/", nullptr, MS_PRIVATE | MS_REC, nullptr))
        return {104, errno};
    
    NsOptions opts = resolveOptionsFromEnv(rootfs.empty());
    
    if (!rootfs.empty()) {
        util::Rootfs root(rootfs);
        if (!root || !setupPreChrootBinds(root, opts, emu, maps))
            return {105, errno};
        if (::fchdir(root.fd()) != 0 || ::chroot(".") != 0 ||
            ::chdir("/") != 0) return {105, errno};
    }
    
    setupPostChrootMounts(opts, rootfs.empty());
    if (!setupPrivateBinfmt(emu)) return {105, errno};
    
    if (cwdInRoot && !cwdInRoot->empty()) {
        if (::chdir(cwdInRoot->c_str()) != 0) {
            recordStep("chdir", false, true, cwdInRoot->c_str());
            return {111, errno};
        }
    }

    NsEnvVars wrapperEnv;
    if (emu) {
        for (const char* name : {"UNROOT_WRAPPER_DEBUG",
                                 "UNROOT_QEMU_NOFD255"}) {
            if (const char* value = ::getenv(name)) wrapperEnv.emplace_back(name, value);
        }
    }
    if (::clearenv() != 0) return {105, errno};
    for (const auto& kv : wrapperEnv) {
        if (::setenv(kv.first.c_str(), kv.second.c_str(), 1) != 0)
            return {105, errno};
    }
    if (envVars) {
        for (const auto& kv : *envVars) {
            if (::setenv(kv.first.c_str(), kv.second.c_str(), 1) != 0)
                return {105, errno};
        }
    }
    
    return {};
}

// The logic for the process that becomes PID 1 in the new namespace.
// NOTE: This function intentionally uses _exit() rather than return/throw paths.
// Rationale:
// - We are executing in a forked child inside new namespaces (user/mount/pid).
// - Using _exit() guarantees we don't run C++ static destructors or atexit() handlers
//   inherited from the parent, which could interact badly with shared state or FDs.
// - It also avoids flushing shared stdio buffers twice across fork.
// - Callers expect exact exit codes to be mapped by the parent via waitpid.
[[noreturn]] void pid1Logic(const std::string& rootfs,
                            const std::vector<std::string>& shellArgv,
                            const EmuPlan* emu,
                            const std::vector<BindMap>* maps,
                            const NsEnvVars* envVars,
                            const std::string* cwdInRoot,
                            int setupWrite) {
    if (gNsVerboseOverride < 0) gNsVerboseOverride = currentVerboseLevel();
    if (gNsJsonOverride < 0) gNsJsonOverride = currentJsonLog() ? 1 : 0;
    UniqueFd setup(setupWrite);
    // Set up namespace environment
    SetupResult result = setupNamespaceEnvironment(rootfs, emu, maps, envVars,
                                                   cwdInRoot);
    if (result.status != 0) {
        errno = result.error;
        reportSetupFailure(setup.get(), result.status);
    }

    // Final exec logic
    std::vector<std::string> argv = shellArgv.empty() ? std::vector<std::string>{"/bin/sh"} : shellArgv;

    int execStatus[2];
    if (::pipe2(execStatus, O_CLOEXEC) != 0) _exit(101);
    UniqueFd execRead(execStatus[0]);
    UniqueFd execWrite(execStatus[1]);

    pid_t tp = ::fork();
    if (tp < 0) _exit(109);
    if (tp == 0) {
        execRead.reset();
        // Child that will perform the final exec. Provide richer diagnostics on failure.
        // Best-effort stat of target before attempting execvp so we can distinguish ENOENT vs ENOEXEC vs EACCES.
        if (!argv.empty()) {
          struct stat st{}; if (::stat(argv[0].c_str(), &st) == 0) {
            char note[128]; std::snprintf(note, sizeof(note), "mode=%o size=%lld", (int)(st.st_mode & 07777), (long long)st.st_size);
            recordStep("exec:stat", true, true, note); // essential so shows at level 1
          } else {
            recordStep("exec:stat", false, true, argv[0].c_str());
          }
        }
        std::vector<char*> cargv; cargv.reserve(argv.size() + 1);
        for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        // On failure capture errno and emit a step before exiting with legacy code 106 (mapped to exec_failed)
        int e = errno; (void)e; // errno captured for recordStep
        {
          // Include errno string in note for clarity
          char note[128]; std::snprintf(note, sizeof(note), "%s errno=%d", cargv[0], e);
          recordStep("exec:fail", false, true, note);
        }
        const char failed = 'E';
        (void)util::write_all(execWrite.get(), &failed, 1);
        _exit(106);
    }

    execWrite.reset();
    char execFailure = 0;
    ssize_t execResult;
    do {
      execResult = ::read(execRead.get(), &execFailure, 1);
    } while (execResult < 0 && errno == EINTR);
    execRead.reset();
    if (execResult == 0) {
      const char complete = 'S';
      if (!util::write_all(setup.get(), &complete, 1)) _exit(101);
    }
    setup.reset();

    int exitCode = 0;
    while (true) {
      int wst = 0;
      pid_t w = ::waitpid(-1, &wst, 0);
      if (w < 0) { if (errno == EINTR) continue; break; }
      if (w == tp) {
        if (WIFEXITED(wst)) exitCode = WEXITSTATUS(wst);
        else if (WIFSIGNALED(wst)) exitCode = 128 + WTERMSIG(wst);
        break;
      }
    }
    _exit(execResult == 0 ? exitCode : 106);
}

[[noreturn]] void reportSetupFailure(int output, int status) {
  int error = errno;
  const char event = 'E';
  (void)util::write_all(output, &event, 1);
  (void)util::write_all(output, &error, sizeof(error));
  _exit(status);
}

// Complete the child side of the parent-authorized ID mapping handshake.
// The parent is the only process which writes ID maps; rich mappings require
// the privileged newuidmap/newgidmap helpers and cannot be self-mapped.
static UniqueFd awaitIdMap(int c2p_write, int p2c_read) {
  UniqueFd cw(c2p_write);
  UniqueFd pr(p2c_read);
  const char ready = 'R';
  if (!util::write_all(cw.get(), &ready, 1)) _exit(101);

  char response = 0;
  if (!util::read_exact(pr.get(), &response, 1)) _exit(101);
  if (response != 'M') _exit(103);
  return cw;
}

// Child (intermediate) process that unshares namespaces, waits for its ID maps,
// and then forks the final PID 1. Intentional _exit() usage:
// - Same reasons as pid1Logic; running in a freshly forked child.
// - Error paths communicate via specific exit codes to the parent.
[[noreturn]] void childLogic(int c2p_write, int p2c_read, bool native,
                             const std::string& rootfs,
                             const std::vector<std::string>& shellArgv,
                             const EmuPlan* emu,
                             const std::vector<BindMap>* maps,
                             const NsEnvVars* envVars,
                             const std::string* cwdInRoot) {
  int flags = CLONE_NEWNS | CLONE_NEWPID;
  if (!native) flags |= CLONE_NEWUSER;
  if (::unshare(flags) != 0)
    reportSetupFailure(c2p_write, native ? 113 : 100);
  UniqueFd setup;
  if (native) {
    ::close(p2c_read);
    setup.reset(c2p_write);
  } else {
    setup = awaitIdMap(c2p_write, p2c_read);
  }

    pid_t rpid = ::fork();
    if (rpid < 0) _exit(108);
    if (rpid > 0) {
      setup.reset();
      int rst = 0; 
      if (::waitpid(rpid, &rst, 0) < 0) _exit(127);
      if (WIFEXITED(rst)) _exit(WEXITSTATUS(rst));
  _exit(127);
  }

  pid1Logic(rootfs, shellArgv, emu, maps, envVars, cwdInRoot,
            setup.release());
}

using IdMapper = std::function<NsResult(pid_t)>;

static NsResult mapSingleId(pid_t childPid, const util::IdMapPlan& plan) {
  auto writeMap = [childPid](const char* name,
                             const util::IdMapExtent& extent) {
    char path[64];
    char map[64];
    std::snprintf(path, sizeof(path), "/proc/%d/%s_map", (int)childPid, name);
    int length = std::snprintf(map, sizeof(map), "%u %u %u\n",
                               extent.inside, extent.outside, extent.count);
    UniqueFd fd(::open(path, O_WRONLY | O_CLOEXEC));
    return fd && util::write_all(fd.get(), map, (size_t)length);
  };
  if (!writeMap("uid", plan.uids.front())) return {2, "failed to write uid_map"};
  // An unprivileged gid_map write requires permanently disabling setgroups.
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)childPid);
  UniqueFd setgroups(::open(path, O_WRONLY | O_CLOEXEC));
  if ((!setgroups && errno != ENOENT) ||
      (setgroups && !util::write_all(setgroups.get(), "deny", 4))) {
    int error = errno;
    return {29, namespacePolicyDiagnostic("setgroups deny failed", error)};
  }
  if (!writeMap("gid", plan.gids.front())) return {2, "failed to write gid_map"};
  return {0, {}};
}

static int runMapHelper(const char* helper, const char* name, pid_t childPid,
                        const std::vector<util::IdMapExtent>& extents) {
  std::vector<std::string> arguments{name, std::to_string(childPid)};
  for (const auto& extent : extents) {
    arguments.push_back(std::to_string(extent.inside));
    arguments.push_back(std::to_string(extent.outside));
    arguments.push_back(std::to_string(extent.count));
  }
  return util::spawn_and_wait_execv(helper, arguments);
}

static NsResult mapRichId(pid_t childPid, const util::IdMapPlan& plan) {
  constexpr const char* uidHelper = "/usr/bin/newuidmap";
  constexpr const char* gidHelper = "/usr/bin/newgidmap";
  if (::access(uidHelper, X_OK) != 0 || ::access(gidHelper, X_OK) != 0) {
    return {24, "rich ID mapping requires newuidmap and newgidmap"};
  }
  if (runMapHelper(uidHelper, "newuidmap", childPid, plan.uids) != 0) {
    return {24, "newuidmap failed"};
  }
  // newgidmap supplies parent authority, so rich roots retain setgroups.
  if (runMapHelper(gidHelper, "newgidmap", childPid, plan.gids) != 0) {
    return {24, "newgidmap failed"};
  }
  return {0, {}};
}

static NsResult decodeChildStatus(int status, int setupErrno = 0) {
  if (!WIFEXITED(status)) return {25, "child signaled"};
  int ec = WEXITSTATUS(status);
  switch (ec) {
    case 0: return {0, {}};
    case 100: return {10, namespacePolicyDiagnostic(
        "unshare failed for user, mount, and PID namespaces", setupErrno)};
    case 101: return {23, "sync failed"};
    case 102: return {2, "failed to write uid_map"};
    case 103: return {24, "ID mapping failed"};
    case 104: return {4, namespacePolicyDiagnostic(
        "failed to set MS_PRIVATE", setupErrno)};
    case 105: return {11, "chroot/chdir failed"};
    case 106: return {12, "exec failed"};
    case 107: return {13, "user bind map failed"};
    case 108: return {26, "pidns fork failed"};
    case 109: return {27, "target fork failed"};
    case 110: return {11, "procfs unavailable after mount failure"};
    case 111: return {28, "cwd change failed"};
    case 113: return {10, namespacePolicyDiagnostic(
        "unshare failed for mount and PID namespaces", setupErrno)};
    default: return {-1, std::to_string(ec)};
  }
}

static NsResult decodeTargetStatus(int status) {
  if (!WIFEXITED(status)) return {25, "child signaled"};
  int exitCode = WEXITSTATUS(status);
  return exitCode == 0 ? NsResult{0, {}}
                       : NsResult{-1, std::to_string(exitCode)};
}

// Parent process: wait for the child, establish its ID maps, notify it, and
// decode its result. All descriptors and the child are consumed on every path.
static NsResult parentLogic(pid_t childPid, int c2pRead, int p2cWrite,
                            const IdMapper& mapper) {
  UniqueFd cr(c2pRead);
  UniqueFd pw(p2cWrite);
  char event = 0;
  bool eventReceived = util::read_exact(cr.get(), &event, 1);
  int setupErrno = 0;
  if (eventReceived && event == 'E')
    (void)util::read_exact(cr.get(), &setupErrno, sizeof(setupErrno));
  bool readyReceived = eventReceived && event == 'R';
  NsResult mapping = readyReceived ? mapper(childPid)
                                   : NsResult{23, "child did not signal readiness"};
  bool notified = false;
  if (readyReceived) {
    char response = mapping.code == 0 ? 'M' : 'X';
    notified = util::write_all(pw.get(), &response, 1);
  }
  pw.reset();

  char setup = 0;
  bool setupReceived = mapping.code == 0 && notified &&
                       util::read_exact(cr.get(), &setup, 1);
  if (setupReceived && setup == 'E')
    (void)util::read_exact(cr.get(), &setupErrno, sizeof(setupErrno));
  bool setupComplete = setupReceived && setup == 'S';
  cr.reset();

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(childPid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != childPid) return {22, "waitpid failed"};
  if (!readyReceived) return decodeChildStatus(status, setupErrno);
  if (mapping.code != 0) return mapping;
  if (!notified) return {23, "failed to notify child after ID mapping"};
  return setupComplete ? decodeTargetStatus(status)
                       : decodeChildStatus(status, setupErrno);
}

static NsResult parentNativeLogic(pid_t childPid, int c2pRead) {
  UniqueFd output(c2pRead);
  char event = 0;
  bool eventReceived = util::read_exact(output.get(), &event, 1);
  int setupErrno = 0;
  if (eventReceived && event == 'E')
    (void)util::read_exact(output.get(), &setupErrno, sizeof(setupErrno));
  output.reset();

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(childPid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != childPid) return {22, "waitpid failed"};
  return eventReceived && event == 'S' ? decodeTargetStatus(status)
                                      : decodeChildStatus(status, setupErrno);
}

} // namespace

NsResult enterNamespace(const std::string& rootfs,
                        const std::vector<std::string>& shellArgv,
                        const util::IdMapPlan& idmap, const EmuPlan* emu,
                        const std::vector<BindMap>* maps,
                        const NsEnvVars* envVars,
                        const std::string* cwdInRoot) {
  std::string invalid = util::validateIdMapPlan(idmap);
  if (!invalid.empty()) return {24, std::move(invalid)};
  const bool native = idmap.mode == util::IdMapMode::Native;
  int c2p[2]; // child -> parent (READY)
  int p2c[2]; // parent -> child (map instruction/complete)
  if (::pipe2(c2p, O_CLOEXEC) != 0) return {20, "pipe failed"};
  if (::pipe2(p2c, O_CLOEXEC) != 0) { ::close(c2p[0]); ::close(c2p[1]); return {20, "pipe failed"}; }
  UniqueFd c2p_r(c2p[0]);
  UniqueFd c2p_w(c2p[1]);
  UniqueFd p2c_r(p2c[0]);
  UniqueFd p2c_w(p2c[1]);

  pid_t pid = ::fork();
  if (pid < 0) {
    return {21, "fork failed"};
  }

  if (pid == 0) {
    // Child: will write READY to c2p_w and read instruction from p2c_r
    c2p_r.reset(); // close unused read end
    p2c_w.reset(); // close unused write end
    childLogic(c2p_w.release(), p2c_r.release(), native, rootfs, shellArgv,
               emu, maps, envVars, cwdInRoot);
  } else {
    // Parent: will read READY from c2p_r and send instruction via p2c_w
    c2p_w.reset(); // close unused write end
    p2c_r.reset(); // close unused read end
    if (native) {
      p2c_w.reset();
      return parentNativeLogic(pid, c2p_r.release());
    }
    return parentLogic(pid, c2p_r.release(), p2c_w.release(),
                       [&idmap](pid_t childPid) {
                         return idmap.mode == util::IdMapMode::Rich
                                    ? mapRichId(childPid, idmap)
                                    : mapSingleId(childPid, idmap);
                       });
  }
  return {99, "unreachable"}; // Should not be reached
}
