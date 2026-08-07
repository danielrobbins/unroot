# Tests

`make test` builds and runs the C++ doctest unit suite. `make e2e` uses pytest
to execute the shipped `bin/unroot` binary and its `bin/unroot-util` sibling
against real Linux namespace and mount primitives. `make e2e-cross` adds
foreign-architecture execution.

Create a Python development environment and enable the commit hooks with:

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements-dev.txt
python3 -m pre_commit install
```

The E2E harness creates temporary root filesystems, captures subprocess output,
enforces timeouts, and verifies that host `binfmt_misc` state remains unchanged
outside tests explicitly intended to qualify native host registration. It also
replaces Unroot's configured `sudo` path with a test guard so an unprivileged
regression cannot modify global registration state.

Archive coverage performs a real GNU tar round trip, checks xattr preservation,
and verifies that `.unroot` metadata cannot cross the archive boundary. The
rich-ID test additionally proves that logical archive ownership survives both
host subordinate-ID translation and a second unpack.

Set `UNROOT_E2E_STRICT=1` for release qualification. In strict mode, missing
fixtures fail instead of skipping. Named platform guarantees can be made
mandatory with
`UNROOT_E2E_REQUIRE=private_binfmt,rich_idmap,native_mode,cross_arch`.
Cross-architecture tests use `CROSS_CC`, or an executable supplied through
`FOREIGN_BINARY`.

Set `UNROOT_E2E_REPORT=path.json` to write a machine-readable host snapshot and
session result. CI uploads these reports as build artifacts so a green or red
matrix cell retains the kernel, architecture, namespace policy, seccomp mode,
and initial `binfmt_misc` state that produced it.

GitHub CI sets Ubuntu's AppArmor user-namespace restriction explicitly. The
restricted cells run `test_namespace_policy.py`; enabled cells run the complete
single-ID rootfs, rich-ID, native-ownership, and cross-architecture suites.

Ubuntu builds the helper with libsubid so the rich-ID suite exercises
provider-backed selection, persisted-map reuse, and exact-allocation
validation. CI also builds and runs `unroot-util` against musl with its
direct-file fallback.

The Docker qualification keeps an ordinary container as a negative control.
Its restricted system-path cell verifies that a denied `newuidmap` write is
reported as an ID-mapping policy failure before rootfs setup can continue.
Its enabled case relaxes AppArmor, seccomp, and Docker's system-path masking so
Unroot can create nested user, mount, and PID namespaces and mount their private
procfs. It does not use `--privileged`; rich ID mapping remains a required
non-container host qualification and may be skipped when the outer Docker runtime refuses the
mapping helpers.
