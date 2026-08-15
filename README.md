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

```console
$ unroot unpack raspi4-rootfs.tar.xz ~/roots/raspi4
$ unroot enter ~/roots/raspi4 -- /bin/sh
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

```console
$ unroot pack ~/roots/raspi4 raspi4-modified.tar.zst
```

### Use a 32-core x86-64 system as an ARM64 build machine

Say `/mnt/raspi4-root` is an NFS mount of a real Raspberry Pi root filesystem.
You want to compile a large project using the target system's own compiler and
libraries, but with the CPU and memory of your workstation:

```console
$ sudo unroot enter --native /mnt/raspi4-root -- /bin/bash
```

Once inside, verify you're running in the ARM64 environment:

```console
# uname -a
Linux workstation 6.6.0 #1 SMP PREEMPT_DYNAMIC aarch64 GNU/Linux
# cat /etc/os-release
NAME="Raspberry Pi OS"
VERSION="12 (bookworm)"
```

You're now in a parallel ARM64 system on your x86-64 desktop. You can't see the
Pi's running processes — this is the Pi's userspace, isolated from your host.
But you can execute any command, use the Pi's package manager, and build software
with the Pi's own toolchain, all while leveraging your workstation's 32 cores and
memory. When you're ready to build:

```console
# cd /home/pi/src/large-project
# make -j32
```

Native mode keeps the mounted filesystem's existing ownership intact. unroot
detects the ARM64 executable, arranges the required host QEMU handler, and runs
the target's own toolchain through QEMU. Unlike rich roots, native mode operates
in the host's user namespace, so the `binfmt_misc` handler is registered
system-wide for the current boot. This isn't cross-compilation: it's the
Raspberry Pi userspace itself, executing on the larger machine. Build output is
written directly to the mounted rootfs.

### Enter a single-owner rootfs without `sudo`

An unmanaged rootfs whose relevant files all belong to your account can use a
single-ID mapping without subordinate UID or GID allocations:

```console
$ unroot enter --single ~/roots/appliance -- /bin/sh
```

Inside that rootfs, your host UID and GID appear as `0`. Other identities cannot
be represented, so this is deliberately not a fallback for a multi-user rootfs.

### Run one process inside another Linux environment

You don't need a VM or container daemon just to run one program against a
different userspace:

```console
$ unroot enter ~/roots/debian-testing --map-ro "$PWD:/work" -- /usr/bin/python3 /work/check-release.py
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

## Three Explicit Modes

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

A rich operation never degrades to single-ID ownership, and an unprivileged
operation never silently becomes native host-root execution.

## Quick Start

Enter a rootfs and start its default shell:

```console
$ unroot unpack raspi4-rootfs.tar.xz ~/rootfs
```

After unpacking, `unroot unpack` also copies in the host's
`/etc/resolv.conf` and `/etc/hosts`, so DNS name resolution works as expected.
It installs `/etc/mtab` as a link to the entered namespace's live mount table.
You can see the active injections as follows:

```console
$ unroot inject list ~/rootfs
NAME            DESTINATION                 OWNER       MODE    ORIGINAL  CURRENT
resolv.conf     /etc/resolv.conf            0:0         0644    regular   present
hosts           /etc/hosts                  0:0         0644    absent    present
mtab            /etc/mtab                   0:0         0777    symlink   symlink
```

The originals they may have overwritten are safely preserved — they
aren't lost, and you can restore them later if needed. When you pack the rootfs,
these injected host-specific files are automatically excluded from the tarball,
keeping the archive portable and independent of your particular host
configuration. See [Managing Rootfs Injections](#managing-rootfs-injections) for
more information related to inspecting, disabling, replacing, or removing these files.

Now, you can enter the rootfs:

```console
$ unroot enter ~/rootfs
#
```

You are now root inside the rootfs, which may even be using QEMU to emulate a
foreign architecture if you are not currently running Linux arm-64bit.

Run a specific command with a clean environment:

```console
$ unroot enter ~/rootfs --cwd /build --env MAKEFLAGS=-j8 -- make
```

Unroot supplies a conventional target-side `PATH` and automatically preserves
the host `TERM` when it is set, so interactive terminal programs usually work
immediately after entry. Use `--persist-env` to copy additional selected host
variables, `--env` to set explicit values, or `--no-default-env` when even the
built-in `PATH` and `TERM` should be omitted.

Enter a single-owner rootfs without subordinate IDs:

```console
$ unroot enter --single ~/rootfs -- /bin/sh
```

**Note:** To use managed rich roots (the default mode for `unpack` and `enter`),
your account needs subordinate UID and GID ranges configured in
`/etc/subuid` and `/etc/subgid`. This is a one-time host setup that enables
unprivileged multi-user chroots. Native mode and rooted `--single` do not
require subordinate IDs. The tradeoff is that you can only be `root` inside
the rootfs.

To configure rich root support, add entries for your username (replace `drobbins`):

```console
$ # View current allocations (if any)
$ getent subuid "$USER"
$ getent subgid "$USER"

$ # Add subordinate ranges (requires root)
$ echo "drobbins:100000:65536" | sudo tee -a /etc/subuid
$ echo "drobbins:100000:65536" | sudo tee -a /etc/subgid
```

Each line has three fields: `username:start_id:count`. This example allocates
IDs 100000–165535 (65,536 IDs) to your account, which is enough for most
rootfs workflows. The kernel then maps these host IDs to the rootfs's users
and groups, preserving multi-user ownership without requiring host root.

The default rich root represents logical IDs 0–65535. If an archive uses a
higher UID or GID, allocate sufficient subordinate IDs and choose the required
headroom when creating the root:

```console
$ unroot unpack --id-count 100000 rootfs.tar.xz ~/roots/large-ids
```

`--id-count` excludes rootfs ID 0, so this example represents IDs 0–100000 and
requires contiguous subordinate UID and GID ranges of at least 100,000 IDs.
The selected map is recorded and revalidated on every later entry.

Foreign architecture execution is automatic — unroot detects the executable type
and selects an appropriate emulator. If QEMU user-mode emulation is already
installed and registered on your host, unroot will reuse it. For rich roots, you
can also select an explicit static emulator and QEMU CPU:

```console 
$ unroot enter ~/arm64-rootfs --qemu /opt/qemu-aarch64-static --qemu-cpu cortex-a53 -- /bin/sh
```

Use `--emulation never` when a foreign target should fail rather than run under
QEMU. Native mode accepts a trusted root-owned emulator path but rejects
`--qemu-cpu`, because it operates in the host user namespace and its
`binfmt_misc` handler is registered system-wide, so it cannot carry per-rootfs
CPU policy.

**Note for Portage users:** Foreign-architecture Portage builds may need
`FEATURES="-pid-sandbox"` in `/etc/portage/make.conf`. After Portage creates
another PID namespace, QEMU linux-user can fail to create threads. This is an
upstream QEMU limitation ([issue #172](https://gitlab.com/qemu-project/qemu/-/issues/172));
native-architecture builds are unaffected.

## Rootfs Archives

Unroot currently supports raw root filesystem tar archives, with filesystem
entries such as `bin/`, `etc/`, and `usr/` directly at the archive root. Gentoo
and Funtoo stage tarballs are common examples. Wrapping directories,
OCI/container-image layouts, disk images, and installer media are not currently
supported; additional formats will likely be added in the future.

`unroot unpack ARCHIVE ROOT` creates a managed rich rootfs and extracts the
archive inside its subordinate-ID mapping. It opens and inspects the archive,
then preflights helper binaries, archive ownership, and ID ranges before
extraction, avoiding partial work when the input or host setup is unsuitable.

Use `sudo unroot unpack --native ARCHIVE ROOT` for host numeric IDs. Both forms
record their ownership mode in `ROOT/.unroot/meta.json`.

`unroot pack ROOT ARCHIVE` validates the recorded mode before capture. It
preserves ownership, permissions, timestamps, links, sparse files, POSIX ACLs,
and extended attributes including file capabilities. SELinux labels are included
when active. Compression is selected from the destination suffix (`.gz`, `.xz`,
`.zst`). Registered injections are replaced by their preserved rootfs originals
during capture.

Archives contain the ownership visible inside the managed root, not its shifted
host representation. This makes `pack` and `unpack` the safe way to copy a rootfs
between ownership models. For example, turn a rich root into a separate native,
host-root-owned tree with:

```console
$ unroot pack ~/roots/gentoo-rich gentoo.tar.zst
$ sudo unroot unpack --native gentoo.tar.zst ~/roots/gentoo-native
```

To go the other way, run `sudo unroot pack` on a managed native root and unpack
the archive normally to create a rich unprivileged copy. The source tree is not
modified during either conversion.

Packing requires a managed rootfs. Unpacking requires an empty destination and
never overlays an existing tree. Host-specific `.unroot` metadata is excluded
from archives; incoming archives containing it are rejected.

## Managing Rootfs Injections

Host files and compatibility links installed into a managed rootfs are called
*injections*. `unpack` registers `hosts`, `resolv.conf`, and `mtab` by default,
preserving whatever was at each destination before installing writable host
copies and the `/etc/mtab` link to `/proc/self/mounts`. The registrations are
durable: later `enter` commands simply use the current files and link.

See what is registered:

```console
$ unroot inject list ~/rootfs
$ unroot inject list ~/rootfs --json
```

Remove one default and restore its original rootfs file, or add it again using
fresh host content:

```console
$ unroot inject remove ~/rootfs hosts
$ unroot inject add ~/rootfs hosts
```

To create a rootfs without one or more defaults, subtract them at unpack time or
remove them afterward with `unroot inject remove`:

```console
$ unroot unpack --inject=-hosts stage3.tar.xz ~/rootfs
$ unroot unpack --inject=-mtab stage3.tar.xz ~/rootfs
$ unroot unpack --inject=-* stage3.tar.xz ~/rootfs
```

You can also register an explicitly authorized host file. This example installs
a root-owned mode-`0600` copy at `/etc/example/config`:

```console
$ unroot inject add ~/rootfs /host/config:/etc/example/config:0:0:0600
```

`unroot inject remove` restores the destination that was preserved when the
registration was first added; `unroot inject clear` restores every registered
destination. `pack` leaves the live rootfs unchanged but writes those preserved
portable originals to the archive, so host-specific files, generated links,
and custom injected content do not leak into the result. See
[ROOTFS INJECTIONS](docs/unroot.md#rootfs-injections) for the complete
file-type, ownership, and lifecycle contract.

Use `unroot inspect archive ARCHIVE` to see the format, ownership range,
layout, and extended metadata without extracting it. `unroot inspect host`
reports the namespace, mapping-helper, and libarchive capabilities used by the
real operations. Both commands support `--json` for diagnostic tooling.

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

For usability, rooted entry also binds the host `/sys` into the namespace as a
read-only tree. Programs such as `lscpu`, `htop`, and build tools that inspect
CPU topology can therefore see the running machine's sysfs data without gaining
writable access to kernel control files.

## A Tool That Respects Your Intelligence ⭐

unroot is designed for root filesystems and commands that you trust. It is not
a general-purpose sandbox for hostile code.

Rich roots and single-ID rootfs entry use a user namespace, so namespace root
capabilities do not become host root privileges. Native mode deliberately uses
host root. Every mode creates private mount and PID namespaces, keeping mount
changes local to the process tree.

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
durable rich and native ownership, explicit single-ID rootfs entry, native and
foreign-architecture commands, full-metadata tar import and export, minimal
automatic mounts, environment and working-directory configuration, and exact
child I/O and exit-status propagation.

Broader experiments such as `fs.*` mutation commands, OCI packaging, overlays,
and persistent sessions are deliberately outside the initial release. See
[Initial Public Release Scope](docs/initial-release-scope.md).

## Installation

Tagged releases provide target-native packages for Debian 13, Ubuntu 24.04
and 26.04, Fedora 44, and Enterprise Linux 9. Each package installs both the
static `unroot` engine and the host-compatible, dynamically linked
`unroot-util` helper required by archive operations and managed rich roots.

Where the target distribution provides one, packages recommend its static QEMU
user-mode provider. Normal `apt` and Fedora `dnf` installations therefore
include cross-architecture support automatically. EL9 does not package a QEMU
user-mode provider in its standard repositories, so users must supply one for
foreign execution. Users who only need single-ID rootfs entry or
same-architecture operation may deliberately disable recommended or weak
dependencies. Unroot does not require a distribution-installed host-global
`binfmt_misc` policy.

Other distributions can build both executables from the release source archive
with `make cli` and install them together with `make install`. A complete
installation always keeps `unroot` and its matching `unroot-util` helper in the
same binary directory.

## Requirements

- Linux with the namespace operations required by the selected mode
- Unprivileged user namespaces for rich roots and `enter --single`
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
- A libarchive-enabled `unroot-util` beside `unroot` for archive inspection,
  `pack`, `unpack`, and injection management
- A C++17 compiler and GNU Make when building from source

`make cli` produces a statically linked `bin/unroot` namespace engine and a
dynamically linked `bin/unroot-util` host-integration helper. `make install`
places both in the same directory. When libsubid's development interface is
available, the helper uses the host's configured subordinate-ID provider;
otherwise it reads `/etc/subuid` and `/etc/subgid`. Archive operations use the
host's libarchive through the same helper. See
[Host Integration Helper](docs/helper-architecture.md).

## Build And Test

```console
$ make cli
$ make check
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
