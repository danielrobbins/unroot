# Host Integration Helper

Unroot deliberately separates its portable namespace engine from host runtime
integration:

- `unroot` is statically linked and owns argument handling, executable
  inspection, namespace creation, mounts, emulation, and process execution.
- `unroot-util` is dynamically linked and performs narrowly defined host
  lookups whose correct behavior may depend on libc, NSS, or another host
  provider.

Single-ID rootfs entry and same-architecture native execution use only the
static engine. Managed rich roots use `unroot-util` for subordinate-ID
selection and exact recorded-allocation validation.

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
executes that exact file. It does not search `PATH`. The helper runs as the
invoking user before namespace setup and has no special privileges.

The two programs exchange one bounded line on a versioned protocol:

```text
unroot-idmap-v1 UID_START GID_START COUNT SOURCE
```

Selection is requested with `idmap --count COUNT`; validation uses
`idmap --validate UID_START GID_START COUNT`. Both return the same record, and
validation succeeds only for the exact requested allocation.

`unroot` rejects a failed helper, excess output, an unknown protocol version,
extra fields, a mismatched count, and ranges that overflow the kernel ID
domain. Diagnostics from a normally failing helper are returned to the user.

## Scope Discipline

The helper is not a second application layer and must not become a general
command runner. An operation belongs there only when it:

1. needs the host's dynamic runtime or provider configuration;
2. can be completed before entering a namespace;
3. does not inspect or mutate the target root filesystem; and
4. leaves namespace, mount, privilege, and child-lifecycle policy in `unroot`.

This gives future host-integration problems a clean outlet without weakening
the static engine's portability or scattering provider-specific conditionals
through its control flow.

## Building And Packaging

`make cli` builds both executables. `make install` places them in the same
binary directory. Distribution packages should install both and provide
libsubid's development interface at build time when the target distribution
supports non-file subordinate-ID providers.

The standalone static release binary remains sufficient for `enter --single`
and same-architecture native execution. Managed rich roots require a host-built
or distribution-built `unroot-util`, because distributing one generic
dynamically linked helper would defeat the purpose of integrating with the
target host runtime. The helper also builds against musl; without a compatible
libsubid interface it uses the local file backend.
