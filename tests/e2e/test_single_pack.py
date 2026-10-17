from __future__ import annotations

import json
import os
import shutil
import subprocess
import tarfile
from pathlib import Path

import pytest

from .support import UnrootRunner


pytestmark = pytest.mark.e2e


@pytest.fixture(params=[False, True])
def single_owner_archive(unroot: UnrootRunner, tmp_path: Path, request):
    root = tmp_path / "single-source"
    root.mkdir()
    payload = root / "payload"
    payload.write_text("built from source\n")
    payload.chmod(0o640)
    os.setxattr(payload, "user.unroot_test", b"preserved")
    (root / "link").symlink_to("payload")
    os.link(payload, root / "hardlink")
    if request.param:
        (root / ".unroot").mkdir()
        (root / ".unroot/meta.json").write_text(json.dumps({"qemuWrapper": {}}))
    before = payload.stat()
    archive = tmp_path / "root.tar.gz"

    unroot.run("pack", "--single", str(root), str(archive)).assert_ok()
    return root, archive, before


def test_single_pack_preserves_root_owned_payload(single_owner_archive) -> None:
    root, archive, before = single_owner_archive
    payload = root / "payload"
    assert (payload.stat().st_uid, payload.stat().st_gid) == (before.st_uid, before.st_gid)
    with tarfile.open(archive) as packed:
        members = packed.getmembers()
        assert all(member.uid == member.gid == 0 for member in members)
        assert not any(member.name.removeprefix("./").startswith(".unroot") for member in members)
        assert any(member.issym() and member.linkname == "payload" for member in members)
        assert any(member.islnk() for member in members)


def test_single_pack_roundtrip_preserves_root_owned_payload(
    empty_rootfs: Path, single_owner_archive, unroot: UnrootRunner, tmp_path: Path
) -> None:
    # Probe rich mapping with the normal fixture before testing the archive.
    _, archive, _ = single_owner_archive
    restored = tmp_path / "restored"
    unroot.run("unpack", "--inject=-*", str(archive), str(restored)).assert_ok()
    assert (restored / "payload").read_text() == "built from source\n"
    assert (restored / "payload").stat().st_mode & 0o777 == 0o640
    assert os.getxattr(restored / "payload", "user.unroot_test") == b"preserved"
    assert (restored / "payload").stat().st_ino == (restored / "hardlink").stat().st_ino


def test_single_pack_refuses_recorded_mapping(
    unroot: UnrootRunner, empty_rootfs: Path, tmp_path: Path
) -> None:
    archive = tmp_path / "bad.tar"
    result = unroot.run("pack", "--single", str(empty_rootfs), str(archive))
    assert result.returncode != 0
    assert "cannot override recorded" in result.stderr
    assert not archive.exists()


@pytest.mark.parametrize("force", [False, True])
def test_single_pack_refuses_unmapped_acl_without_replacing_archive(
    unroot: UnrootRunner, tmp_path: Path, force: bool
) -> None:
    setfacl = shutil.which("setfacl")
    if not setfacl:
        pytest.skip("setfacl is required")
    root = tmp_path / "source"
    root.mkdir()
    payload = root / "payload"
    payload.write_text("content")
    subprocess.run([setfacl, "-m", f"u:{os.getuid() + 12345}:r", str(payload)], check=True)
    archive = tmp_path / "existing.tar"
    archive.write_bytes(b"previous archive")
    result = unroot.run("pack", "--single", *(["--force"] if force else []), str(root), str(archive))
    assert result.returncode != 0
    assert "cannot represent file or ACL ownership" in result.stderr
    assert archive.read_bytes() == b"previous archive"
    assert not list(tmp_path.glob(".unroot-archive-*"))


@pytest.mark.parametrize("owner", ["0", ":0"])
def test_single_pack_refuses_unmapped_file_owner(
    unroot: UnrootRunner, tmp_path: Path, privileged_prefix: tuple[str, ...], owner: str
) -> None:
    if os.getuid() == 0 or os.getgid() == 0:
        pytest.skip("requires a non-root caller")
    root = tmp_path / "source"
    root.mkdir()
    payload = root / "payload"
    payload.write_text("content")
    subprocess.run([*privileged_prefix, "chown", owner, str(payload)], check=True)
    archive = tmp_path / "bad.tar"
    result = unroot.run("pack", "--single", str(root), str(archive))
    assert result.returncode != 0
    assert "cannot represent file or ACL ownership" in result.stderr
    assert not archive.exists()
