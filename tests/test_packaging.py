import platform

import pytest

from scripts import package_release


@pytest.mark.parametrize(
    ("target", "expected"),
    [
        ("ubuntu-24.04", "1.0~beta1-1~ubuntu24.04.1"),
        ("ubuntu-26.04", "1.0~beta1-1~ubuntu26.04.1"),
        ("debian-13", "1.0~beta1-1~debian13.1"),
    ],
)
def test_debian_versions_sort_prereleases_before_final(target, expected):
    metadata = package_release.TARGETS[target]

    assert package_release.debian_version(
        "1.0_beta1", metadata.package_suffix
    ) == expected


def test_rpm_versions_keep_prerelease_in_release_component():
    assert package_release.rpm_version("1.0_beta1") == (
        "1.0",
        "0.1.beta1",
    )
    assert package_release.rpm_version("1.0") == ("1.0", "1")


def test_package_targets_use_static_qemu_provider_for_older_deb_and_rpm():
    assert package_release.TARGETS["ubuntu-24.04"].qemu_package == (
        "qemu-user-static"
    )
    assert package_release.TARGETS["ubuntu-26.04"].qemu_package == "qemu-user"
    assert package_release.TARGETS["debian-13"].qemu_package == "qemu-user"
    assert package_release.TARGETS["fedora-44"].qemu_package == (
        "qemu-user-static"
    )
    assert package_release.TARGETS["el-9"].qemu_package is None


def test_package_build_refuses_cross_architecture_host(monkeypatch):
    requested = package_release.normalized_architecture(platform.machine())
    machine = "aarch64" if requested == "x86_64" else "x86_64"
    monkeypatch.setattr(package_release.platform, "machine", lambda: machine)

    with pytest.raises(ValueError, match="build host"):
        package_release.verify_native_architecture(requested)


@pytest.mark.parametrize("version", ["v1.0", "1.0-beta1", "1", "1.0_beta 1"])
def test_package_versions_reject_unsupported_forms(version):
    with pytest.raises(ValueError, match="unsupported VERSION"):
        package_release.version_parts(version)
