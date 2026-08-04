# Unroot Source

Unroot is a C++17 program built as one static `bin/unroot` executable. The
initial public interface provides `enter`, `pack`, and `unpack` actions.

## Core Flow

- `unroot.cpp`, `app_core.cpp`, and `program_context.cpp` own process startup,
  global options, and top-level command dispatch.
- `actions/enter_config.cpp` and `actions/enter_action.cpp` turn the command
  line into one namespace-entry request.
- `actions/archive_config.cpp` and `actions/archive_action.cpp` capture and
  restore tar archives under the rootfs's persisted ID map.
- `linuxns.cpp` owns user, mount, and PID namespace creation, ID-map
  authorization, rootfs setup, and target-process lifecycle.
- `util/rootfs.cpp` is the descriptor-relative boundary for host-side access to
  a target rootfs. Pre-`chroot` code must not reconstruct rootfs paths itself.

## Architecture Emulation

- `arch.cpp` and `shebang.cpp` identify the actual target interpreter and ELF
  machine, class, and byte order. `arch.cpp` also provides the small canonical
  mapping from that identity to a QEMU user-mode executable.
- `emulation.cpp`, `binfmt.cpp`, and `wrapper.cpp` prepare private
  `binfmt_misc` registration and static QEMU execution inside the namespace.
  `emulation::Manager` is the sole owner of native-versus-foreign execution
  policy and QEMU validation.
- `compat_blacklist.cpp` applies known host-kernel and emulator compatibility
  policy.

## Supporting Services

- `meta.cpp` records rootfs metadata through the contained rootfs API.
- `hostcaps.cpp` gathers diagnostic host capabilities.
- `diagnostics.cpp` reports fatal errors and observable namespace-policy
  context.
- `util/` and `include/util/` provide narrow internal services for file
  descriptors, process I/O, paths, ID ranges, and errors.

The private development repository retains earlier `fs.*` experiments for
future filesystem operations. They are neither linked into the `1.0_beta1`
binary nor included in public source snapshots.

## Validation

`make test` runs the C++ doctest suite. `make e2e` uses pytest to exercise the
production binary against real Linux namespaces. See
[`docs/testing.md`](../docs/testing.md) for the qualification matrix.
