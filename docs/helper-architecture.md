# Host Integration Helper

Unroot deliberately separates its portable namespace engine from host runtime
integration:

- `unroot` is statically linked and owns argument handling, executable
  inspection, namespace creation, mounts, emulation, and process execution.
- `unroot-util` is dynamically linked and provides narrowly defined services
  whose correct behavior depends on the host runtime: account-provider
  lookups and libarchive-backed inspection, packing, and extraction.

Single-ID rootfs entry and same-architecture native execution use only the
static engine. Managed rich roots use `unroot-util` for subordinate-ID
selection and exact recorded-allocation validation. Archive operations use its
libarchive service under every ownership mode.

## Why The Split Exists

User lookup through glibc is not reliably self-contained in a static binary:
NSS providers can be loaded dynamically at runtime. Subordinate-ID allocation
can likewise come from libsubid providers rather than directly from
`/etc/subuid` and `/etc/subgid`. Embedding these concerns in the static engine
would either produce misleading static-link behavior or require Unroot to
reimplement host account policy.

`unroot-util` is built against the host runtime instead. If the libsubid
development interface is available, it asks libsubid for the current user's
UID and GID ranges. Otherwise it falls back to strict parsing of the local
subuid and subgid files, accepting either the username or numeric UID form.
The helper selects a contiguous range when a rich rootfs is initialized. On
later entries it verifies that the exact recorded range remains assigned; it
does not select a replacement. `newuidmap` and `newgidmap` remain the system
authorities that install and finally validate the requested mappings.

## Trust Boundary

The engine resolves `unroot-util` beside its own `/proc/self/exe` path and
executes that exact file. It does not search `PATH`. The helper has no special
host privileges.

The two programs exchange one bounded line on a versioned protocol:

```text
unroot-idmap-v1 UID_START GID_START COUNT SOURCE
```

Selection is requested with `idmap --count COUNT`; validation uses
`idmap --validate UID_START GID_START COUNT`. Both return the same record, and
validation succeeds only for the exact requested allocation. `COUNT` is the
subordinate extent length and excludes rootfs ID 0. The protocol supports the
kernel ID domain through ID 4294967294; no mapped extent may include the
unmappable `(uid_t)-1`/`(gid_t)-1` sentinel value.

`unroot` rejects a failed helper, excess output, an unknown protocol version,
extra fields, a mismatched count, and ranges that overflow the kernel ID
domain. Diagnostics from a normally failing helper are returned to the user.

Archive inspection uses a separate bounded protocol:

```text
unroot-archive-v1 JSON
```

The engine opens and pins the archive before invoking `archive inspect --fd
FD`. The helper accepts only the inherited descriptor, scans headers through
libarchive, and returns objective archive facts. It never receives an
archive-controlled host pathname.

For `archive pack` and `archive unpack`, the static engine first owns all
policy: root pinning and locking, archive inspection, ID-map selection,
namespace creation, overwrite rules, and atomic output publication. It then
runs the helper inside that selected user, mount, and PID namespace without
chrooting it, with the pinned root as its working directory. This is the same
host-runtime model previously used for GNU tar: the dynamic loader and host
libraries remain available, while relative archive traversal sees the logical
IDs established by the namespace. The archive itself is passed as an inherited
descriptor.

## Scope Discipline

The helper is not a second application layer and must not become a general
command runner. Provider operations belong there only when they:

1. needs the host's dynamic runtime or provider configuration;
2. can be completed before entering a namespace;
3. do not inspect or mutate the target root filesystem; and
4. leave namespace, mount, privilege, and child-lifecycle policy in `unroot`.

Archive traversal is the deliberate second profile. It belongs in the helper
because libarchive is a host-runtime dependency, but it may run only after the
engine has fixed every policy input. It receives descriptors and operation
flags, uses the current working directory selected by the engine, and reports
warnings or failure. It must not select roots, mappings, namespaces,
overwrite policy, or publication policy.

This gives future host-integration problems a clean outlet without weakening
the static engine's portability or scattering provider-specific conditionals
through its control flow.

## Building And Packaging

`make cli` builds both executables. `make install` places them in the same
binary directory. Distribution packages should install both, build the helper
with libarchive, and provide libsubid's development interface when the target
distribution supports non-file subordinate-ID providers.

The standalone static release binary remains sufficient for `enter --single`
and same-architecture native execution. Managed rich roots and archive
operations require a host-built or distribution-built `unroot-util`, because
distributing one generic dynamically linked helper would defeat the purpose of
integrating with the target host runtime. The helper also builds against musl;
without a compatible libsubid interface it uses the local subordinate-ID file
backend.
