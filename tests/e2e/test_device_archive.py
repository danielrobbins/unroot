"""Device creation limits and archive preservation without creating devices."""

import errno
import io
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest


pytestmark = pytest.mark.e2e


@pytest.fixture
def rootless_namespace(require_capability):
    if os.geteuid() == 0:
        pytest.skip("requires an ordinary host user, not host root")
    unshare = shutil.which("unshare")
    require_capability(bool(unshare), "unshare is required", "userns")
    prefix = [unshare, "--user", "--map-root-user", "--"]
    probe = subprocess.run(prefix + ["true"], capture_output=True, text=True,
                           env={**os.environ, "LC_ALL": "C"})
    restricted = {
        "unshare: unshare failed: Operation not permitted",
        "unshare: unshare failed: Permission denied",
        "unshare: unshare failed: No space left on device",
    }
    require_capability(
        not (probe.returncode == 1 and probe.stderr.strip() in restricted),
        "unprivileged user namespaces unavailable: " + probe.stderr, "userns",
    )
    assert probe.returncode == 0, probe.stderr
    return prefix


def device_archive(path: Path, devices: bool) -> None:
    with tarfile.open(path, "w") as archive:
        directory = tarfile.TarInfo("dev")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        archive.addfile(directory)
        marker = tarfile.TarInfo("dev/upstream-marker")
        marker.mode = 0o644
        marker.size = 9
        archive.addfile(marker, io.BytesIO(b"upstream\n"))
        if devices:
            for name, kind, major, minor in (
                ("null", tarfile.CHRTYPE, 1, 3),
                ("console", tarfile.CHRTYPE, 5, 1),
                ("test-block", tarfile.BLKTYPE, 7, 199),
            ):
                entry = tarfile.TarInfo("dev/" + name)
                entry.type = kind
                entry.devmajor, entry.devminor = major, minor
                entry.mode = 0o600
                archive.addfile(entry)


@pytest.mark.parametrize("namespaced", [False, True])
@pytest.mark.parametrize("kind", ["S_IFCHR", "S_IFBLK"])
def test_rootless_mknod_requires_host_privilege(
    tmp_path, rootless_namespace, namespaced, kind
):
    prefix = rootless_namespace if namespaced else []
    program = (
        "import errno, os, stat, sys\n"
        "try:\n"
        "    os.mknod(sys.argv[1], getattr(stat, sys.argv[2]) | 0o600, "
        "os.makedev(1, 3))\n"
        "except OSError as error:\n"
        "    print(error.errno)\n"
        "else:\n"
        "    print('created')\n"
    )
    result = subprocess.run(
        prefix + [sys.executable, "-c", program, str(tmp_path / "node"), kind],
        capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == str(errno.EPERM), result.stdout
    assert not (tmp_path / "node").exists()


@pytest.mark.parametrize("devices", [False, True])
@pytest.mark.parametrize("force", [False, True])
def test_rootless_archive_device_extraction(
    tmp_path, rootless_namespace, unroot_binary, devices, force
):
    archive = tmp_path / "input.tar"
    device_archive(archive, devices)
    root = tmp_path / "root"
    root.mkdir()
    helper = unroot_binary.with_name("unroot-util")
    with archive.open("rb") as source:
        result = subprocess.run(
            rootless_namespace + [
                str(helper), "archive", "unpack", "--fd", str(source.fileno()),
            ] + (["--force"] if force else []),
            cwd=root, pass_fds=(source.fileno(),), capture_output=True,
            text=True, check=False, env={**os.environ, "LC_ALL": "C"},
        )
    assert (root / "dev/upstream-marker").read_bytes() == b"upstream\n"
    if devices:
        # Direct helper extraction has no preservation policy; the CLI adds it.
        assert result.returncode == (0 if force else 1), result.stderr
        assert "dev/null" in result.stderr, result.stderr
        for name in ("null", "console", "test-block"):
            assert not (root / "dev" / name).exists()
    else:
        assert result.returncode == 0, result.stderr
    print(f"devices={devices} force={force}: exit={result.returncode} {result.stderr.strip()}")


def test_native_device_archive_round_trip(tmp_path, unroot_binary, privileged_prefix):
    archive = tmp_path / "input.tar"
    device_archive(archive, True)
    root = tmp_path / "root"
    output = tmp_path / "output.tar"
    try:
        for arguments in (
            ["unpack", "--native", "--inject=-*", str(archive), str(root)],
            ["pack", str(root), str(output)],
        ):
            result = subprocess.run(
                [*privileged_prefix, str(unroot_binary), *arguments],
                capture_output=True, text=True, check=False,
            )
            assert result.returncode == 0, result.stdout + result.stderr
        with tarfile.open(output) as packed:
            members = {entry.name.removeprefix("./"): entry for entry in packed}
            for name, kind, major, minor in (
                ("null", tarfile.CHRTYPE, 1, 3),
                ("console", tarfile.CHRTYPE, 5, 1),
                ("test-block", tarfile.BLKTYPE, 7, 199),
            ):
                entry = members["dev/" + name]
                assert (entry.type, entry.devmajor, entry.devminor, entry.mode) == (
                    kind, major, minor, 0o600,
                )
    finally:
        # Only remove the fresh test root; its native-owned metadata is private.
        if root.exists():
            cleanup = subprocess.run(
                [*privileged_prefix, "rm", "-rf", "--", str(root)],
                capture_output=True, text=True, check=False,
            )
            assert cleanup.returncode == 0, cleanup.stderr
