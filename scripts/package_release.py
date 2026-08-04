#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime
import email.utils
import os
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = Path(tempfile.gettempdir()) / "unroot-release-packages"
DEFAULT_OUTPUT = ROOT / "dist" / "packages"
VERSION_PATTERN = re.compile(
    r"^(?P<base>[0-9]+\.[0-9]+(?:\.[0-9]+)?)(?:_(?P<qualifier>[A-Za-z0-9]+))?$"
)


@dataclass(frozen=True)
class PackageTarget:
    family: str
    distribution: str
    package_suffix: str
    qemu_package: str | None
    rpm_dist: str = ""


TARGETS = {
    "debian-13": PackageTarget("deb", "trixie", "debian13", "qemu-user"),
    "ubuntu-24.04": PackageTarget(
        "deb", "noble", "ubuntu24.04", "qemu-user-static"
    ),
    "ubuntu-26.04": PackageTarget(
        "deb", "resolute", "ubuntu26.04", "qemu-user"
    ),
    "fedora-44": PackageTarget(
        "rpm", "fedora", "fc44", "qemu-user-static", ".fc44"
    ),
    "el-9": PackageTarget("rpm", "enterprise-linux", "el9", None, ".el9"),
}

ARCHITECTURES = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "aarch64": "arm64",
    "arm64": "arm64",
}
DEB_ARCHITECTURES = {"x86_64": "amd64", "arm64": "arm64"}
RPM_ARCHITECTURES = {"x86_64": "x86_64", "arm64": "aarch64"}


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True)


def current_version() -> str:
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"unsupported VERSION format: {version}")
    return version


def version_parts(version: str) -> tuple[str, str | None]:
    match = VERSION_PATTERN.fullmatch(version)
    if not match:
        raise ValueError(f"unsupported VERSION format: {version}")
    return match.group("base"), match.group("qualifier")


def debian_version(version: str, package_suffix: str) -> str:
    base, qualifier = version_parts(version)
    upstream = base if qualifier is None else f"{base}~{qualifier}"
    return f"{upstream}-1~{package_suffix}.1"


def rpm_version(version: str) -> tuple[str, str]:
    base, qualifier = version_parts(version)
    release = "1" if qualifier is None else f"0.1.{qualifier}"
    return base, release


def normalized_architecture(machine: str) -> str:
    try:
        return ARCHITECTURES[machine.lower()]
    except KeyError as error:
        raise ValueError(f"unsupported build architecture: {machine}") from error


def verify_native_architecture(requested: str) -> None:
    actual = normalized_architecture(platform.machine())
    if actual != requested:
        raise ValueError(
            f"package requested for {requested} on {actual} build host"
        )


def stage_source(target_name: str) -> tuple[Path, Path]:
    target_root = BUILD_ROOT / target_name
    source = target_root / "source"
    shutil.rmtree(target_root, ignore_errors=True)
    target_root.mkdir(parents=True)
    ignored = shutil.ignore_patterns(
        ".git",
        ".pytest_cache",
        ".ruff_cache",
        "__pycache__",
        "bin",
        "build",
        "coverage",
        "dist",
        "e2e-reports",
    )
    shutil.copytree(ROOT, source, ignore=ignored)
    return target_root, source


def render(template: Path, output: Path, values: dict[str, str]) -> None:
    content = template.read_text(encoding="utf-8")
    for name, value in values.items():
        content = content.replace(f"@{name}@", value)
    unresolved = sorted(set(re.findall(r"@[A-Z_]+@", content)))
    if unresolved:
        raise ValueError(f"unresolved package template values: {unresolved}")
    output.write_text(content, encoding="utf-8")


def source_date_epoch() -> int:
    configured = os.environ.get("SOURCE_DATE_EPOCH")
    if configured is not None:
        try:
            return int(configured)
        except ValueError as error:
            raise ValueError("SOURCE_DATE_EPOCH must be an integer") from error
    if shutil.which("git"):
        result = subprocess.run(
            ["git", "show", "-s", "--format=%at", "HEAD"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode == 0:
            return int(result.stdout.strip())
    return int((ROOT / "VERSION").stat().st_mtime)


def changelog_date() -> str:
    date = datetime.datetime.fromtimestamp(
        source_date_epoch(), tz=datetime.timezone.utc
    )
    return email.utils.format_datetime(date)


def build_deb(
    target_name: str,
    target: PackageTarget,
    architecture: str,
    output_dir: Path,
) -> list[Path]:
    version = current_version()
    target_root, source = stage_source(target_name)
    debian = source / "debian"
    shutil.copytree(source / "packaging" / "debian", debian)
    values = {
        "DEBIAN_VERSION": debian_version(version, target.package_suffix),
        "DISTRIBUTION": target.distribution,
        "QEMU_RECOMMENDS": f"Recommends: {target.qemu_package}",
        "CHANGELOG_DATE": changelog_date(),
    }
    render(debian / "control.in", debian / "control", values)
    render(debian / "changelog.in", debian / "changelog", values)
    (debian / "control.in").unlink()
    (debian / "changelog.in").unlink()
    (debian / "rules").chmod(0o755)

    actual = subprocess.run(
        ["dpkg", "--print-architecture"],
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    expected = DEB_ARCHITECTURES[architecture]
    if actual != expected:
        raise ValueError(f"dpkg architecture is {actual}, expected {expected}")

    env = os.environ.copy()
    env["DEB_BUILD_OPTIONS"] = f"parallel={os.cpu_count() or 1}"
    run(["dpkg-buildpackage", "-b", "-us", "-uc"], source, env)
    packages = sorted(target_root.glob("unroot_*.deb"))
    if len(packages) != 1:
        raise ValueError(f"expected one Debian package, found {len(packages)}")
    return copy_outputs(packages, output_dir)


def source_archive(source: Path, output: Path, version: str) -> None:
    epoch = source_date_epoch()

    def normalize(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.uid = 0
        info.gid = 0
        info.uname = "root"
        info.gname = "root"
        info.mtime = epoch
        return info

    with tarfile.open(output, "w:xz", format=tarfile.PAX_FORMAT) as archive:
        archive.add(source, arcname=f"unroot-{version}", filter=normalize)


def build_rpm(
    target_name: str,
    target: PackageTarget,
    architecture: str,
    output_dir: Path,
) -> list[Path]:
    version = current_version()
    target_root, source = stage_source(target_name)
    topdir = target_root / "rpmbuild"
    for directory in ("BUILD", "BUILDROOT", "RPMS", "SOURCES", "SPECS", "SRPMS"):
        (topdir / directory).mkdir(parents=True)
    archive = topdir / "SOURCES" / f"unroot-{version}.tar.xz"
    source_archive(source, archive, version)
    package_version, package_release = rpm_version(version)
    spec = topdir / "SPECS" / "unroot.spec"
    render(
        source / "packaging" / "rpm" / "unroot.spec.in",
        spec,
        {
            "SOURCE_VERSION": version,
            "RPM_VERSION": package_version,
            "RPM_RELEASE": package_release,
            "QEMU_RECOMMENDS": (
                f"Recommends:     {target.qemu_package}"
                if target.qemu_package
                else ""
            ),
        },
    )
    run(
        [
            "rpmbuild",
            "-bb",
            str(spec),
            "--define",
            f"_topdir {topdir}",
            "--define",
            f"dist {target.rpm_dist}",
        ],
        source,
    )
    expected = RPM_ARCHITECTURES[architecture]
    packages = sorted((topdir / "RPMS" / expected).glob("unroot-*.rpm"))
    if len(packages) != 1:
        raise ValueError(f"expected one RPM package, found {len(packages)}")
    return copy_outputs(packages, output_dir)


def copy_outputs(packages: list[Path], output_dir: Path) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = []
    for package in packages:
        output = output_dir / package.name
        shutil.copy2(package, output)
        outputs.append(output)
    return outputs


def build_package(target_name: str, architecture: str, output_dir: Path) -> list[Path]:
    verify_native_architecture(architecture)
    target = TARGETS[target_name]
    if target.family == "deb":
        return build_deb(target_name, target, architecture, output_dir)
    return build_rpm(target_name, target, architecture, output_dir)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build target-native Unroot release packages."
    )
    parser.add_argument("--target", required=True, choices=sorted(TARGETS))
    parser.add_argument(
        "--architecture", required=True, choices=sorted(set(ARCHITECTURES.values()))
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    for package in build_package(args.target, args.architecture, args.output_dir):
        print(package)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
