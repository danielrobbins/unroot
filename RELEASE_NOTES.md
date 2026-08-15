# Unroot 1.0.5

**Bug Fix Release** -- August 15, 2026.

Unroot 1.0.5 fixes a root mount regression introduced in `1.0.4` while preserving unprivileged `unroot enter` behavior.

- Rootfs mount setup was corrected so the entered root consistently behaves as `/`, including when paths traverse through top-level directories such as `/etc/../proc`. This restores compatibility with relative `/etc/mtab` links and preserves visibility of existing nested submounts beneath the target root. Regression coverage now explicitly checks both preserved submount visibility and relative `/etc/mtab` compatibility. ([#36](https://github.com/danielrobbins/unroot/issues/36))

# Unroot 1.0.4

**Feature and Bug Fix Release** -- August 14, 2026.

Unroot 1.0.4 strengthens Linux distribution compatibility, closes a remaining archive metadata-safety gap and adds progress bars for packing and unpacking!

- An entered rootfs now appears as `/` in its own mount table, giving package managers and other tools the conventional root mount entry they expect. ([#33](https://github.com/danielrobbins/unroot/issues/33))
- `/etc/mtab` is now a default, reversible injection pointing to `/proc/self/mounts`. Any original file or symlink is preserved for removal and packing. ([#34](https://github.com/danielrobbins/unroot/issues/34))
- `unroot` source builds now perform a build-time check to ensure that `libarchive` is built with support for POSIX ACLs and extended attributes. This addresses a bug that could cause silent metadata loss. ([#32](https://github.com/danielrobbins/unroot/issues/32))
- GitHub release downloads no longer include standalone `unroot` binaries. A functional installation needs both the static `unroot` engine and its host-linked `unroot-util` helper, so it's recommended to use a prebuilt distro package or build from source.
- Interactive archive inspection, unpacking, and packing now display compact, terminal-aware progress with average throughput in MiB/s.

# Unroot 1.0.3

**Features Release** — August 13, 2026

A number of improvements have been made to `unroot` to improve usability and make the tool more seamless in real-world use, as well as more robust. Many thanks to Zeno R. R. Davatz (@zdavatz) for extended testing and comprehensive issue reporting. Unroot would not be the same without community support and feedback!

- `$TERM` is now preserved inside the rootfs by default which generally allows ncurses programs and colors to work seamlessly in the terminal after `unroot enter`.
- `/sys` is recursively bind-mounted read-only into the rootfs by default, allowing tools such as `lscpu` and `htop` to function properly without exposing writable sysfs control files.
- Unroot now uses `libarchive` through the sibling `unroot-util` host helper. This removes the dependency on a locally-installed GNU tar and eliminates a class of bugs.
- Parallel `xz` compression is now supported and enabled by default via `libarchive`.
- `unroot unpack --id-count COUNT` can create rich roots with ownership headroom beyond the default IDs 0–65535. ([#19](https://github.com/danielrobbins/unroot/issues/19))
- New `inspect host` and `inspect archive <archive>` actions expose runtime capabilities and archive facts ([#21](https://github.com/danielrobbins/unroot/issues/21)).
- `unroot unpack` will always do a single pre-flight scan of the to-be-unpacked archive; when it finds POSIX ACLs or extended attributes, `unpack` now performs a quick check for the corresponding support on the exact destination filesystem before extraction begins. If necessary features are not present, `unpack` aborts prior to extraction. This can be overridden with `--force`.
- `/etc/resolv.conf` and `/etc/hosts` are now *injected* (safely copied) into managed (non-native) rootfs environments using the novel `inject` action. They were previously read-only bind-mounted so could not be altered. This new method allows local name resolution to work while allowing local edits to the files, and prevents local network configuration from leaking into packed rootfs tarballs. This also provides a framework for customizing other injectable files that you want inserted into the unpacked rootfs by default, but don't want to become part of the packed/distributed rootfs.
- `pack` and `unpack` now take one self-cleaning exclusive lock per rootfs, so archive reads and writes cannot overlap on the same tree. A competing command fails immediately, while process exit automatically releases ownership and leaves no stale lock file. ([#27](https://github.com/danielrobbins/unroot/issues/27))
- For now, OCI images are rejected until we add support. ([#25](https://github.com/danielrobbins/unroot/issues/25))
- `enter` rejects paths that resolve to the host `/`.

# Unroot 1.0.2

**Maintenance Release** — August 7, 2026

Unroot 1.0.2 fixes the archive metadata check introduced in 1.0.1. The check
now asks GNU tar to process a real temporary file, ensuring older tar builds
report missing ACL or extended-metadata support before `pack` or `unpack`
proceeds. Without this fix, some tar builds reported the limitation only while
processing the real rootfs, after the preflight had already passed.

Hosts whose GNU tar lacks requested metadata support continue to fail closed by
default. Use `--force` when reduced archive fidelity is understood and
acceptable. ([#15](https://github.com/danielrobbins/unroot/issues/15))

# Unroot 1.0.1

**Maintenance Release** — August 6, 2026

Unroot 1.0.1 incorporates the first round of real-world feedback after the
initial release.

This release strengthens robustness, streamlines the user experience, improves
the CLI interface, and enhances diagnostics and archive safety.

Highlights include removing the standalone `single` command while adding
`enter --single` for unmanaged rootfs trees, adding essential device nodes to
`/dev` for better tool compatibility, fixing a `setgroups` issue affecting
Gentoo (Gentoo users should still set `FEATURES="-pid-sandbox"` due to a
QEMU/Portage incompatibility), providing a sensible default `PATH` to simplify
command invocation, and making `--map-ro` more convenient for common use cases.
Full details below:

## Rootfs Compatibility

- `/dev` now includes `/dev/full`, standard stream links, and proper PTY
  support. Shell process substitution and interactive tools work correctly
  without exposing the entire host `/dev` tree.
  ([#2](https://github.com/danielrobbins/unroot/issues/2))

- Commands now receive a sensible default `PATH` focused on the rootfs. You can
  still customize it explicitly, or use `--no-default-env` for a completely
  empty environment when needed.
  ([#3](https://github.com/danielrobbins/unroot/issues/3))

- Rich roots now preserve supplementary group memberships, allowing tools like
  Portage to drop privileges correctly. The safety restriction on `setgroups`
  remains in place for single-ID mappings only.
  ([#6](https://github.com/danielrobbins/unroot/issues/6))

- When namespace setup fails, error messages now clearly explain what went
  wrong and why, including helpful context for AppArmor, seccomp, SELinux, or
  container-related issues.
  ([#7](https://github.com/danielrobbins/unroot/issues/7))

## Ownership And CLI

- `unroot enter --single ROOT` lets you enter unmanaged, single-owner rootfs
  trees without needing subordinate UID/GID allocations.
  ([#4](https://github.com/danielrobbins/unroot/issues/4))

- `--map-ro SOURCE` is now shorthand for `--map-ro SOURCE:SOURCE` — simpler
  when the host and container paths match.
  ([#5](https://github.com/danielrobbins/unroot/issues/5))

- The standalone `unroot single` command has been removed to reduce confusion.
  Single-ID namespace root is now available through `unroot enter --single`
  when entering a specific rootfs.
  ([#12](https://github.com/danielrobbins/unroot/issues/12),
  [#14](https://github.com/danielrobbins/unroot/issues/14))

- Native mode is now described more clearly as Unroot's conventional privileged
  chroot workflow for host-owned and mounted filesystems.
  ([#11](https://github.com/danielrobbins/unroot/issues/11))

## Archive Safety And Ownership Conversion

- `pack` and `unpack` now check whether your tar installation supports ACLs,
  extended attributes, and SELinux labels. Unroot refuses to silently lose
  metadata by default; use `--force` if you accept reduced fidelity.
  ([#8](https://github.com/danielrobbins/unroot/issues/8))

- Documentation now explains how to convert between rich and native roots:
  `pack` followed by `unpack --native` creates a native tree from a rich root,
  while the reverse flow creates a managed rich copy without manual ownership
  changes.
  ([#10](https://github.com/danielrobbins/unroot/issues/10))

## Known QEMU Compatibility Boundary

Foreign Portage builds may need `FEATURES="-pid-sandbox"` because QEMU
linux-user cannot create a host thread after Portage enters another PID
namespace. Native-architecture builds are unaffected. Unroot documents this
upstream limitation but does not silently alter distribution policy.
([#13](https://github.com/danielrobbins/unroot/issues/13),
[QEMU #172](https://gitlab.com/qemu-project/qemu/-/issues/172))

# Unroot 1.0.0

**Initial Release** — August 3, 2026

unroot is a small, daemonless toolkit for entering, modifying, and transporting
Linux root filesystems. It brings together rootless multi-user chroots,
automatic QEMU emulation, and full-metadata archive transport in one binary.

Originally developed as the namespace engine for FFS, unroot is now a focused
Kernel Seeds project with its own release boundary and test matrix.

## What's New

### Three Explicit Modes

unroot keeps ownership choices explicit rather than guessing:

- **Managed rich roots** — Unprivileged multi-user roots using subordinate UID/GID
  ranges. Perfect for build and packaging work without host root.
- **Native roots** — Host-owned or mounted filesystems with native ownership.
  Requires `sudo` for conventional chroot behavior.
- **Single-ID roots** — Enter unmanaged single-owner rootfs trees without
  subordinate IDs by explicitly using `unroot enter --single ROOT`.

unroot never silently downgrades rich ownership to single-ID or escalates an
unprivileged request into host-root execution.

### Rootfs Archives

- `unroot unpack ARCHIVE ROOT` — Extract a tar archive into a managed rootfs
  with correct ownership mapping. Rich preservation is the default.
- `unroot pack ROOT ARCHIVE` — Capture a rootfs with full metadata: permissions,
  timestamps, links, sparse files, ACLs, xattrs, file capabilities, and SELinux
  labels.
- Automatic compression from suffix (`.gz`, `.xz`, `.zst`, etc.)
- Host-specific `.unroot` metadata excluded from portable archives

### Foreign Architecture Execution

- Automatic architecture detection for ELF binaries and shebang interpreters
- Private QEMU emulation for rich roots — active only inside the rootfs
- Host-wide emulation for native mode (current boot only)
- Explicit refusal with `--emulation never` or manual selection with
  `--qemu` and `--qemu-cpu`
- Validated across x86-64 ↔ ARM64 in both directions

unroot handles execution compatibility. You remain responsible for distro
profiles, compiler flags, and CPU baselines.

### Environment Control

- `--cwd` — Set working directory inside the rootfs
- `--env VAR=value` — Set explicit environment variables
- `--persist-env VAR` — Copy selected variables from the host
- `--map-ro HOST:CONTAINER` — Read-only host path mappings
- Clean environment by default; bare commands require explicit `PATH`

## Security Model

unroot is designed for **trusted** build, packaging, and rootfs-maintenance
workloads. It is **not** a hostile-code sandbox:

- ✓ Private mount and PID namespaces
- ✓ User namespace isolation (rich roots and single-ID rootfs entry)
- ✓ Namespace root ≠ host root
- ✗ Shared network, IPC, hostname, cgroups, and kernel
- ✗ No syscall filters, resource limits, or MAC policies

Use unroot with root filesystems and commands you trust.

## What's Included

**Source:** A release archive for building the static `unroot` engine and its
host-compatible `unroot-util` helper together.

**Packages:** Native `.deb` and `.rpm` packages for:
- Debian 13 (Bookworm)
- Ubuntu 24.04 LTS and 26.04
- Fedora 44
- Enterprise Linux 9 (Rocky Linux 9)

Each native package installs both `unroot` and `unroot-util` with the helper's
required host-library dependencies.

**QEMU:** Packages recommend the distribution's static QEMU user-mode emulator
for multi-architecture support, but it's not required — unroot works without it
for native roots. EL9 users should install QEMU separately if foreign execution
is needed.

All artifacts are built, tested, and validated by release CI before publication.

## Known Limitations

- First release focuses on entry and transport — no OCI packaging, overlays,
  or persistent sessions yet
- Native mode registers host-wide `binfmt_misc` handlers (current boot only)
- Requires Linux 5.12+ for `--map-ro`, 6.7+ for private rich-mode foreign
  execution
- Subordinate UID/GID allocation required for rich roots (configured by
  distribution in `/etc/subuid` and `/etc/subgid`)

See [Initial Public Release Scope](docs/initial-release-scope.md) for details
on what's deliberately outside this release.

## Getting Started

```bash
# Enter a rootfs
unroot enter ~/rootfs

# Unpack an ARM64 Raspberry Pi rootfs on your x86-64 workstation
unroot unpack raspi4-rootfs.tar.xz ~/roots/raspi4
unroot enter ~/roots/raspi4

# Enter a single-owner rootfs without subordinate IDs
unroot enter --single ~/roots/appliance -- /bin/sh
```

For complete usage examples, see [README.md](README.md).
