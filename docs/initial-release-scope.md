# Initial Public Release Scope

The initial public release establishes Unroot as a focused, dependable toolkit
for entering, running, and transporting Linux root filesystems. It supports
both unprivileged subordinate-ID ownership and explicit host-native ownership
without attempting to publish every experiment retained on the `post-funtoo`
branch.

## Supported Core

- `unroot enter ROOT [OPTIONS] -- COMMAND [ARGUMENTS...]`
- `unroot enter --single ROOT [OPTIONS] -- COMMAND [ARGUMENTS...]`
- `sudo unroot enter --native ROOT [OPTIONS] -- COMMAND [ARGUMENTS...]`
- `unroot unpack ARCHIVE ROOT`
- `sudo unroot unpack --native ARCHIVE ROOT`
- `unroot pack ROOT ARCHIVE [OPTIONS]`
- durable rich subordinate UID and GID mappings for multi-user ownership,
  persisted per rootfs and revalidated before every entry;
- native host ownership for privileged chroots, existing root filesystems, and
  mounted roots that must retain their host-visible IDs;
- private mount and PID namespaces in every mode, plus a private user namespace
  for rich roots and single-ID rootfs entry;
- automatic QEMU execution for supported foreign architectures, with explicit
  controls to disable emulation or select a static emulator and CPU model;
- private `binfmt_misc` registration for rich roots and guarded host registration
  for native foreign roots;
- full-metadata tar extraction and capture under the durable ownership model;
- explicit working-directory and environment configuration;
- automatic minimal `/proc`, `/dev`, and resolver setup;
- child standard I/O and exit-status propagation; and
- a static namespace engine plus a narrow host-linked helper used only for rich
  subordinate-ID discovery and validation.

The ownership modes are not fallbacks for one another. A failed rich setup must
not collapse a multi-user rootfs into one identity, and an unprivileged request
must not silently become native host-root execution.

## Deferred Surface

The following work remains private while it is completed and tested. It is not
linked into the initial release or included in its public source snapshot:

- `fs.*` mutation commands and the Unroot JSON Shell;
- OCI image construction and non-tar packaging;
- overlays, layers, and persistent sessions; and
- higher-level orchestration previously explored for Funtoo and FFS.

Deferred features can be promoted individually after their behavior,
documentation, and end-to-end tests meet the same standard as `enter`.

## Release Standard

The public story, README, manual, help output, and release artifacts must
describe the supported core consistently. Experimental history and private
strategy documents are not shipped merely because they exist in Git. The test
matrix in [testing.md](testing.md) defines the minimum execution evidence for a
release.

Unroot owns execution compatibility, not distro architecture policy. It
detects the requested command's real ELF architecture and selects a QEMU
user-mode executable when needed. Rootfs variants, compiler flags, package
profiles, and CPU baselines belong to the distribution or build system that
produced the rootfs.
