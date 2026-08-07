# unroot — The Linux Root Filesystem Super-Tool

unroot lets your Linux system enter, modify, build within, and transport any
other Linux root filesystem. Different CPU architecture? No problem. Need to
become root without `sudo`? Done. Want to move an entire Linux system between
machines as a single archive? That too.

Think of it as a `chroot`, container daemon, cross-compiler, and system migration
tool had a baby — except it's just one small binary with no background services.

## Why unroot Exists

Working with Linux root filesystems usually means juggling multiple tools:

```
Traditional approach:
  chroot setup          → manual, error-prone
  + qemu-user-static    → host-wide binfmt_misc configuration
  + tar                 → lose metadata without careful flags
  + sudo                → host root access for everything
  + hand-crafted scripts → glue it all together

unroot approach:
  unroot enter ~/rootfs → done
```

unroot brings together everything you need to work with Linux root filesystems:
rootless multi-user chroots, automatic QEMU emulation, rootfs archive transport,
and namespace-isolated root builds — all in one daemonless tool.

No container daemons. No hand-crafted chroot setups. No host-wide emulation
configuration. Just one binary that does exactly what you need.

## What Can You Do With It?

### Modify a Raspberry Pi rootfs on a Ryzen workstation

You've downloaded an ARM64 Raspberry Pi rootfs archive, but you're sitting at
a much faster x86-64 Ryzen workstation. Instead of cross-compiling or booting
the Pi, enter the rootfs directly:

```bash
unroot unpack raspi4-rootfs.tar.xz ~/roots/raspi4
unroot enter ~/roots/raspi4 -- /bin/sh
```

`unpack` preserves the rootfs's users, groups, permissions, capabilities, and
extended attributes using your subordinate UID and GID ranges. (These are extra
numeric identities allocated to your account in `/etc/subuid` and `/etc/subgid` —
unroot maps them to the rootfs's users and groups, so you can represent a
multi-user system without needing host root.) When `/bin/sh` turns out to be
ARM64, unroot automatically selects a static QEMU AArch64 emulator — or reuses
an existing QEMU installation if one is already registered — and registers it
in a private `binfmt_misc` namespace. (This means the emulator is active only
inside that rootfs, not system-wide. Your host's emulation configuration stays
unchanged.) From the shell, you can use the Raspberry Pi distribution's own
package manager, compiler, and tools as though you were running natively.

When you're done, capture the modified system:

```bash
unroot pack ~/roots/raspi4 raspi4-modified.tar.zst
```

### Use a 32-core x86-64 system as an ARM64 build machine

Say `/mnt/raspi4-root` is an NFS mount of a real Raspberry Pi root filesystem.
You want to compile a large project using the target system's own compiler and
libraries, but with the CPU and memory of your workstation:

```bash
sudo unroot enter --native /mnt/raspi4-root \
    -- /bin/bash
```

Once inside, verify you're running in the ARM64 environment:

```bash
bash$ uname -a
Linux workstation 6.6.0 #1 SMP PREEMPT_DYNAMIC aarch64 GNU/Linux
bash$ cat /etc/os-release
NAME="Raspberry Pi OS"
VERSION="12 (bookworm)"
```

You're now in a parallel ARM64 system on your x86-64 desktop. You can't see the
Pi's running processes — this is the Pi's userspace, isolated from your host.
But you can execute any command, use the Pi's package manager, and build software
with the Pi's own toolchain, all while leveraging your workstation's 32 cores and
memory. When you're ready to build:

```bash
bash$ cd /home/pi/src/large-project
bash$ make -j32
```

Native mode keeps the mounted filesystem's existing ownership intact. unroot
detects the ARM64 executable, arranges the required host QEMU handler, and runs
the target's own toolchain through QEMU. Unlike rich roots, native mode operates
in the host's user namespace, so the `binfmt_misc` handler is registered
system-wide for the current boot. This isn't cross-compilation: it's the
Raspberry Pi userspace itself, executing on the larger machine. Build output is
written directly to the mounted rootfs.

### Become root for a trusted build without `sudo`

Some embedded and system builds expect to run as root even when they don't need
root access to the host. `single` maps your ordinary UID and GID to `0` inside
a private user namespace:

```bash
mkdir -p "$HOME/roots/appliance"
unroot single \
    --cwd "$PWD" \
    --persist-env PATH \
    --env DESTDIR="$HOME/roots/appliance" \
    -- /bin/sh -c 'make -j32 && make install'
```

Inside the command, `id -u` returns `0`, root checks succeed, and you have
namespace-root capabilities. On the host, you're still your ordinary user —
mount and PID state stay private, and newly installed files remain manageable
by your regular account.

This mode needs an unprivileged user namespace but doesn't need subordinate UID
or GID allocations. It's perfect for embedded systems where nearly every file
is owned by root. If your build needs multiple users or service accounts, use
a managed rich root instead.

The same one-ID mapping can be used while changing the filesystem root. This is
useful for an unmanaged rootfs whose relevant files all belong to your account:

```bash
unroot enter --single ~/roots/appliance -- /bin/sh
```

Inside that rootfs, your host UID and GID appear as `0`. Other identities cannot
be represented, so this is deliberately not a fallback for a multi-user rootfs.

### Run one process inside another Linux environment

You don't need a VM or container daemon just to run one program against a
different userspace:

```bash
unroot enter ~/roots/debian-testing \
    --map-ro "$PWD:/work" \
    -- /usr/bin/python3 /work/check-release.py
```

The process sees the selected rootfs as `/`, gets private mount and PID state,
exits normally, and leaves no daemon behind.

## The unroot Difference

```
┌─────────────────────────────────────────────────────────────┐
│  Without unroot:                                            │
│                                                             │
│  chroot setup    → manual prep, root required               │
│  QEMU config     → host-wide binfmt_misc, one-size-fits-all │
│  Archive extract → lose metadata without careful tar flags  │
│  Multi-user      → either root or broken ownership          │
│  Cross-arch      → manual emulator selection                │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  With unroot:                                               │
│                                                             │
│  unroot enter ~/rootfs                                      │
│    ↓                                                        │
│    • Automatic QEMU for foreign architectures               │
│    • Multi-user ownership without host root (rich mode)     │
│    • Full metadata preservation (ACLs, xattrs, capabilities)│
│    • Private namespaces, no daemon                          │
│    • Explicit modes — no guessing                           │
└─────────────────────────────────────────────────────────────┘
```

## Who Is unroot For?

**Embedded developers** — Build, modify, and test Raspberry Pi, embedded ARM,
or RISC-V root filesystems on your x86-64 workstation without cross-compilation
guesswork.

**Distro builders** — Create and maintain multi-user root filesystems as an
ordinary user, preserving complete ownership and metadata without host root.

**System migrators** — Transport entire Linux systems between machines as
single archives with full fidelity, including capabilities and ACLs.

**Container skeptics** — Get namespace isolation and rootfs flexibility without
daemon overhead, image formats, or orchestration complexity.

**Kernel developers** — Run one process inside another userspace for testing,
validation, or tooling without booting VMs or managing containers.

## Explicit Ownership Modes

Linux root filesystems don't all have the same ownership requirements. unroot
keeps those choices explicit rather than guessing or silently falling back:

- **Managed rich roots** (default) — Created by `unroot unpack`. Rootfs UID/GID
  0 map to you, while subordinate ID ranges preserve multi-user ownership.
  (Subordinate IDs are extra numeric identities allocated to your account in
  `/etc/subuid` and `/etc/subgid` — unroot maps them to the rootfs's users and
  groups, letting you represent a complete multi-user system without host root.)
  The mapping is recorded in `.unroot/meta.json` and revalidated on every entry.
  Perfect for unprivileged multi-user build and packaging work.

- **Native roots** — Host-visible ownership unchanged. Use
  `sudo unroot unpack --native` or `sudo unroot enter --native` for host-owned,
  NFS-mounted, or existing rootfs trees. Conventional privileged chroot behavior.

- **Single-ID roots** — Enter an unmanaged rootfs with
  `unroot enter --single ROOT`. Your host UID and GID map to root, with no
  subordinate IDs required. This is useful for single-owner build roots, but
  cannot represent multiple users or groups.

The standalone `unroot single -- COMMAND` action uses the same one-ID mapping
without changing `/`: the host filesystem remains visible. It is useful for
trusted builds that need namespace-root capabilities rather than another rootfs.

A rich operation never degrades to single-ID ownership, and an unprivileged
operation never silently becomes native host-root execution.

## Quick Start

Enter a rootfs and start its default shell:

```bash
unroot enter ~/rootfs
```

Run a specific command with a clean environment:

```bash
unroot enter ~/rootfs \
    --cwd /build \
    --env MAKEFLAGS=-j8 \
    -- make
```

Unroot supplies a conventional target-side `PATH` but does not inherit the
host environment. Use `--persist-env` to copy selected host variables,
`--env` to set explicit values, or `--no-default-env` when even the built-in
`PATH` should be omitted.

Run a trusted build as namespace root:

```bash
unroot single --persist-env PATH -- /usr/bin/make -j8
```

Enter a single-owner rootfs without subordinate IDs:

```bash
unroot enter --single ~/roots/appliance -- /bin/sh
```

**Note:** To use managed rich roots (the default mode for `unpack` and `enter`),
your account needs subordinate UID and GID ranges configured in
`/etc/subuid` and `/etc/subgid`. This is a one-time host setup that enables
unprivileged multi-user chroots. Native mode, rooted `--single`, and the
standalone `single` action do not require subordinate IDs.

To configure rich root support, add entries for your username (replace `drobbins`):

```bash
# View current allocations (if any)
getent subuid "$USER"
getent subgid "$USER"

# Add subordinate ranges (requires root)
echo "drobbins:100000:65536" | sudo tee -a /etc/subuid
echo "drobbins:100000:65536" | sudo tee -a /etc/subgid
```

Each line has three fields: `username:start_id:count`. This example allocates
IDs 100000–165535 (65,536 IDs) to your account, which is enough for most
rootfs workflows. The kernel then maps these host IDs to the rootfs's users
and groups, preserving multi-user ownership without requiring host root.

Foreign architecture execution is automatic — unroot detects the executable type
and selects an appropriate emulator. If QEMU user-mode emulation is already
installed and registered on your host, unroot will reuse it. For rich roots, you
can also select an explicit static emulator and QEMU CPU:

```bash
unroot enter ~/arm64-rootfs \
    --qemu /opt/qemu-aarch64-static \
    --qemu-cpu cortex-a53 \
    -- /bin/sh
```

Use `--emulation never` when a foreign target should fail rather than run under
QEMU. Native mode accepts a trusted root-owned emulator path but rejects
`--qemu-cpu`, because it operates in the host user namespace and its
`binfmt_misc` handler is registered system-wide, so it cannot carry per-rootfs
CPU policy.

These controls affect execution only. You remain responsible for selecting
distro profiles, compiler flags, and CPU baselines.

## Rootfs Archives

`unroot unpack ARCHIVE ROOT` creates a managed rich rootfs and extracts the
archive inside its subordinate-ID mapping. It preflights helper binaries and ID
ranges before creating `ROOT`, avoiding partial extraction when the host isn't
configured for rich ownership.

Use `sudo unroot unpack --native ARCHIVE ROOT` for host numeric IDs. Both forms
record their ownership mode in `ROOT/.unroot/meta.json`.

`unroot pack ROOT ARCHIVE` validates the recorded mode before capture. It
preserves ownership, permissions, timestamps, links, sparse files, POSIX ACLs,
and extended attributes including file capabilities. SELinux labels are included
when active. Compression is selected from the destination suffix (`.gz`, `.xz`,
`.zst`).

Packing requires a managed rootfs. Unpacking requires an empty destination and
never overlays an existing tree. Host-specific `.unroot` metadata is excluded
from archives; incoming archives containing it are rejected.

## How unroot Works

unroot detects the ELF class, byte order, and machine type of executables,
resolving scripts through their shebang. It doesn't assume every command is an
ELF binary and rejects unsupported combinations rather than selecting an
approximate emulator.

**Rich roots** use a private `binfmt_misc` instance and stage the static emulator
and unroot wrapper beneath `.unroot/bin`. Because rich roots run in a private
user namespace, the emulator is registered only inside that namespace — it
disappears with the process tree and never affects your host system.

**Native roots** use host `binfmt_misc`: unroot reuses an enabled handler with
matching ELF identity, or registers `unroot-ARCH` from a trusted static QEMU
executable. It never replaces a conflicting handler. Because native mode
operates in the host user namespace, the handler is registered system-wide and
remains active for the current boot.

There is no resident daemon or container image format. unroot provides the
appropriate path for the environment in front of you.

## A Tool That Respects Your Intelligence ⭐

unroot is designed for root filesystems and commands that you trust. It is not
a general-purpose sandbox for hostile code.

Rich roots and single mode use a user namespace, so namespace root capabilities
do not become host root privileges. Native mode deliberately uses host root.
Every mode creates private mount and PID namespaces, keeping mount changes local
to the process tree.

unroot shares the host network, IPC namespace, hostname, cgroup hierarchy,
kernel, and hardware interfaces. It does not install syscall filters, impose
resource limits, or create AppArmor or SELinux policies. Mapped paths and
selected devices remain backed by host resources.

**What you get:**
- **Transparency** — Explicit modes, no guessing, no silent fallbacks
- **Visibility** — Metadata recorded in `.unroot/meta.json`, revalidated on entry
- **Cooperation** — Works with your existing workflows, not against them
- **Simplicity** — One binary, no daemon, no hand-crafted setup

## Initial Release Scope

The first public release focuses on dependable rootfs entry and transport:
durable rich and native ownership, explicit single-mode execution, native and
foreign-architecture commands, full-metadata tar import and export, minimal
automatic mounts, environment and working-directory configuration, and exact
child I/O and exit-status propagation.

Broader experiments such as `fs.*` mutation commands, OCI packaging, overlays,
and persistent sessions are deliberately outside the initial release. See
[Initial Public Release Scope](docs/initial-release-scope.md).

## Installation

Tagged releases provide standalone static `unroot` binaries for x86-64 and
ARM64, plus target-native packages for Debian 13, Ubuntu 24.04 and 26.04,
Fedora 44, and Enterprise Linux 9. The native packages install both `unroot`
and the host-compatible `unroot-util` helper required by managed rich roots.

Where the target distribution provides one, packages recommend its static QEMU
user-mode provider. Normal `apt` and Fedora `dnf` installations therefore
include cross-architecture support automatically. EL9 does not package a QEMU
user-mode provider in its standard repositories, so users must supply one for
foreign execution. Users who only need `single` or same-architecture operation
may deliberately disable recommended or weak dependencies. Unroot does not
require a distribution-installed host-global `binfmt_misc` policy.

The standalone binary remains useful for `single` and same-architecture native
operation. Build from source or install a native package for the complete rich
rootfs feature set.

## Requirements

- Linux with the namespace operations required by the selected mode
- Unprivileged user namespaces for rich roots and `single`
- `unroot-util`, `/usr/bin/newuidmap`, `/usr/bin/newgidmap`, and suitable host
  subordinate-ID allocations for rich roots (configured in `/etc/subuid` and
  `/etc/subgid` by your distribution)
- Host root privileges for native roots
- Linux 5.12 or newer when using `--map-ro`
- Linux 6.7 or newer and a compatible static QEMU user emulator for private
  rich-mode foreign execution (or an existing QEMU installation with
  `binfmt_misc` handlers already registered)
- Host `binfmt_misc` and a trusted static QEMU emulator for native foreign
  execution when no compatible handler is already present
- GNU tar for `pack` and `unpack`, plus the compression program selected by an
  archive suffix when creating compressed output
- A C++17 compiler and GNU Make when building from source

`make cli` produces a statically linked `bin/unroot` namespace engine and a
dynamically linked `bin/unroot-util` host-integration helper. `make install`
places both in the same directory. When libsubid's development interface is
available, the helper uses the host's configured subordinate-ID provider;
otherwise it reads `/etc/subuid` and `/etc/subgid`. See
[Host Integration Helper](docs/helper-architecture.md).

## Build And Test

```bash
make cli
make check
```

`make test` runs the fast C++ suite. `make e2e` exercises real namespaces using
a static BusyBox fixture. `make check` runs both. Release validation covers WSL,
Proxmox-hosted Linux, bare-metal Linux, containers, x86-64, ARM64, and both
primary cross-architecture directions; see [Testing Unroot](docs/testing.md).

## Origins

Unroot grew out of work on FFS, where it provides unprivileged rootfs entry and
cross-architecture execution during Linux system builds. The public project is
now being honed into a focused Kernel Seeds tool with its own release boundary.

## License

Unroot is licensed under the GNU General Public License, version 3 only
(`GPL-3.0-only`). Vendored third-party components retain their original
licenses and copyright notices.
