# Testing Unroot

Unroot's core behavior depends on Linux namespaces, mount policy, executable
formats, and host security settings. Unit tests are necessary, but a public
release also needs real execution evidence from multiple Linux environments.

## Test Targets

### `make test`

Builds and runs the self-contained C++ test suite. The target must return a
failure when any test fails; test failures are never ignored.

Tests belong in `tests/dt_<area>.cpp`. Keep fixtures local and deterministic,
and test pure policy at the narrowest useful boundary.

The subordinate-ID tests separately exercise the helper's file backend and the
static core's bounded, versioned helper protocol. They use synthetic files and
executables so malformed provider data and hostile helper output are covered
without depending on the developer's host allocation.

### `make e2e`

Builds `bin/unroot` and runs the pytest suite under `tests/e2e/` against real
Linux namespaces. Python is only the test orchestrator: every behavior is
exercised through the shipped Unroot executable. The suite verifies:

- explicit single-mode ID mapping and host-filesystem mutation guards;
- rich subordinate ID mapping, host ownership translation, durable metadata,
  implicit map reuse, and refusal of stale or conflicting mappings;
- managed rich entry and privileged native ownership and entry;
- working-directory and environment propagation;
- rooted `PATH` command lookup;
- execution of a rooted shebang script;
- a real distribution lifecycle covering unpack, enter, modification, pack,
  restore, and re-entry on both native and foreign architectures;
- child exit-status propagation;
- private, writable `binfmt_misc` mounting; and
- preservation of host-global `binfmt_misc` state in tests that do not
  explicitly qualify native host registration.

Native host-registration policy is unit-tested against a temporary synthetic
registry, including compatible-handler reuse, missing fixed-interpreter flags,
name conflicts, and exact registration output. Ordinary E2E runs do not mutate
the live host registry. A dedicated disposable-host cell is required before
live native foreign registration becomes a release qualification gate.

The test requires unprivileged user namespaces and a static BusyBox. Set
`BUSYBOX=/path/to/busybox` if a static `busybox` is not on `PATH`.
Install the Python test tooling with
`python3 -m pip install -r requirements-dev.txt`.

The real-rootfs journey is offline. Supply checksum-verified Alpine minirootfs
archives through `UNROOT_E2E_ALPINE_NATIVE` and
`UNROOT_E2E_ALPINE_FOREIGN`, with their expected Alpine architecture names in
the corresponding `_ARCH` variables. CI pins the archives and verifies their
SHA-256 checksums before pytest starts. Release cells require the
`rootfs_journey` capability; ordinary local runs skip either role whose fixture
is not configured.

The harness removes inherited `UNROOT_*` settings, supplies a deliberately
failing `UNROOT_SUDO` helper, captures stdout and stderr, and kills the complete
subprocess group on timeout. An E2E regression must therefore fail rather than
prompting for privileges or modifying the host.

### `make e2e-cross`

Runs a foreign static executable through Unroot. Set `CROSS_CC` to a
static-capable cross compiler, or provide an existing executable with
`FOREIGN_BINARY`. `FOREIGN_NAME`, `FOREIGN_ARGS`, and `FOREIGN_EXPECTED` can
describe a prebuilt fixture. The suite executes the foreign ELF directly and
as the interpreter named by a shebang script, ensuring architecture discovery
does not assume that the requested command is itself an ELF executable. It
also verifies that `--emulation never` refuses the target and that an explicit
static emulator and `FOREIGN_QEMU_CPU` specification reach QEMU through the persisted
wrapper configuration.

### Strict mode

Ordinary development runs skip tests whose external fixture is unavailable.
Release jobs set `UNROOT_E2E_STRICT=1`, turning missing fixtures into failures.
`UNROOT_E2E_REQUIRE` names platform guarantees such as `private_binfmt`,
`native_mode`, and `cross_arch`; a matrix cell fails if a required guarantee is
unavailable.
Supported cells must never report success by silently skipping their defining
namespace or emulation contract.

Set `UNROOT_E2E_REPORT=path.json` to write a JSON qualification record. The
record includes the session exit status and a host snapshot containing the
kernel, machine architecture, effective IDs, seccomp and no-new-privileges
state, user-namespace and AppArmor policy settings, and visible `binfmt_misc`
state. GitHub Actions uploads the qualification reports for every matrix cell.

GitHub's Ubuntu matrix explicitly runs with AppArmor user namespaces both
restricted and enabled. Restricted cells prove that Unroot reports policy
denial with the failed operation, system error, and observed AppArmor setting.
Enabled cells run the complete unprivileged and cross-architecture contract as the
ordinary runner user. Runtime diagnostics also report disabled user-namespace
sysctls and an enforcing SELinux context when those facts are visible.

### `make check`

Runs the unit and end-to-end suites. This is the local release gate.

CI builds `unroot-util` against libsubid on Ubuntu and verifies both allocation
selection and exact recorded-range validation before rich-ID E2E uses that
provider-aware path. A separate Alpine job builds and executes the helper
against musl with the direct-file backend. The static `unroot` binary is checked
independently and must not acquire a dynamic interpreter.

Coverage helpers remain available as `make doctest-coverage`,
`make doctest-coverage-all`, and `make doctest-diff-coverage`.

## Release Matrix

WSL is a useful development target, but it is not authoritative for Linux
namespace behavior. Before a public release, `make check` must pass across:

- Funtoo under WSL;
- a conventional Linux VM hosted by Proxmox;
- bare-metal x86-64 Linux;
- x86-64 Linux through GitHub Actions; and
- ARM64 Linux through GitHub Actions.

Container qualification is tracked separately because a kernel may support a
feature that the outer runtime blocks. Test at least:

- an unprivileged Proxmox LXC with nesting enabled;
- Docker with its default policy, which must fail with a useful diagnostic if
  required namespace operations are blocked; and
- a documented Docker configuration that permits Unroot's namespace and mount
  operations.

GitHub CI exercises both Docker cases against a strict public export. The
default profile must reject namespace creation with a classified diagnostic.
The enabled cell removes Docker's AppArmor and seccomp restrictions while
leaving the process as an ordinary user:

```console
docker run --rm \
  --security-opt apparmor=unconfined \
  --security-opt seccomp=unconfined \
  --security-opt systempaths=unconfined \
  unroot-e2e make e2e
```

This is a qualification configuration, not a claim that Unroot provides a
security boundary inside the outer container. Rich ID mapping remains a
required non-container host qualification; the ordinary Docker cell reports it as
unavailable when the outer runtime refuses `newuidmap` or `newgidmap`.

Cross-architecture tests must also exercise both directions:

- x86-64 host to ARM64 rootfs; and
- ARM64 host to x86-64 rootfs.

The matrix supplies a conservative QEMU CPU model for each direction. This
qualifies Unroot's runtime plumbing only; it does not claim that Unroot owns or
validates the rootfs's distro-specific CPU baseline.

Each enabled matrix cell also runs the complete rootfs journey against
same-architecture and foreign Alpine minirootfs archives. This complements the focused static
probe by exercising a distribution's dynamic loader, libraries, shell, and
package metadata through the public CLI.

`make e2e-cross` performs this check with a static binary produced by the
cross compiler named in `CROSS_CC`. CI runs it in both directions after the
native suite, with the corresponding static QEMU user emulator installed. A
prebuilt static fixture can instead be supplied through `FOREIGN_BINARY`, with
`FOREIGN_ARGS` and `FOREIGN_EXPECTED` defining its invocation and output.

Record the distribution, kernel, host architecture, target architecture,
compiler, effective UID, seccomp mode, user-namespace settings, binfmt state,
and test result for each run. A failure remains a platform-policy or Unroot
defect until its cause is understood.

## Current Validation

| Environment | Host | Target | Result |
| --- | --- | --- | --- |
| Funtoo WSL2, Linux 6.6.87.2, GCC 12.3 | ARM64 | ARM64 | same-architecture E2E passes; private `binfmt_misc` unavailable |
| Debian 13 on Proxmox, Linux 7.0.6-2-pve, GCC 14.2 | x86-64 | x86-64 | same-architecture and private `binfmt_misc` E2E tests pass |
| Debian 13 on Proxmox, Linux 7.0.6-2-pve, QEMU 10.0 | x86-64 | ARM64 | direct foreign ELF and foreign shebang E2E tests pass |
| GitHub-hosted Ubuntu 24.04 | x86-64 | x86-64 and ARM64 | native, rich-ID, private `binfmt_misc`, and cross-architecture E2E pass |
| GitHub-hosted Ubuntu 24.04 | ARM64 | ARM64 and x86-64 | native, rich-ID, private `binfmt_misc`, and cross-architecture E2E pass |

The WSL ARM64 result remains useful as a distinct compatibility target rather
than a substitute for native ARM64 Linux. GitHub's native ARM64 runner now
qualifies ARM64-to-x86-64 execution independently.

## Test Design

- Mock pure process boundaries in unit tests; exercise actual namespace and
  mount behavior in E2E tests.
- Invoke `bin/unroot` as a subprocess; do not import or reproduce production
  namespace behavior in Python.
- Snapshot host-global state around tests that mount or register binary formats.
- Run the primary qualification suite as an ordinary user. Root execution can
  be a separate compatibility cell but cannot establish the unprivileged
  contract.
- Do not widen production APIs merely to make a test easier.
- Prefer temporary roots and local fixtures over host-global state.
- Add a regression test with every correctness fix.
- Treat tests as inexpensive; keep the shipped implementation lean.
