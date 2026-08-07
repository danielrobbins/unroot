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

**Binaries:**
- `unroot` — Static namespace engine (x86-64 and ARM64)
- `unroot-util` — Dynamic helper for rich roots (included in packages)

**Packages:** Native `.deb` and `.rpm` packages for:
- Debian 13 (Bookworm)
- Ubuntu 24.04 LTS and 26.04
- Fedora 44
- Enterprise Linux 9 (Rocky Linux 9)

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
