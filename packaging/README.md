# Release Packages

Tagged releases build target-native packages for Debian 13, Ubuntu 24.04 and
26.04, Fedora 44, and Enterprise Linux 9 on x86-64 and ARM64. Each package
contains the static `unroot` namespace engine, the dynamically linked
`unroot-util` host-integration helper, the manual, and user documentation.

QEMU user-mode emulation is a recommended dependency where the target's
standard repositories provide it. Normal `apt` and Fedora `dnf` installations
therefore include cross-architecture support automatically, while minimal
installations can omit weak or recommended dependencies when only
same-architecture or single-ID rootfs operation is needed. EL9 does not package QEMU
user-mode emulation in its standard repositories, so its Unroot package leaves
the optional emulator installation to the user. Unroot does not require a
host-global `binfmt_misc` policy package.

Package builds must run natively inside the named target distribution:

```console
python3 scripts/package_release.py \
    --target ubuntu-26.04 \
    --architecture x86_64
```

The output is written under `dist/packages/`. GitHub Actions installs every
resulting package through the target distribution's package manager, verifies
the installed Unroot version and executable linkage, and confirms that the
recommended QEMU installation provides a static emulator for the opposite
primary architecture.
