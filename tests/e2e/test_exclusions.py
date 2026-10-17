from __future__ import annotations

import fcntl
import json
import os
import tarfile
from pathlib import Path

import pytest

from .support import UnrootRunner

pytestmark = pytest.mark.e2e


def contents(archive: Path) -> set[str]:
    with tarfile.open(archive) as packed:
        return {entry.name.removeprefix("./").rstrip("/") for entry in packed}


def rules(unroot: UnrootRunner, root: Path) -> list[str]:
    result = unroot.run("exclude", "list", str(root), "--json").assert_ok()
    return json.loads(result.stdout)["patterns"]


def test_exclusions_leave_live_files_untouched_and_can_be_removed(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    root = tmp_path / "root"
    files = ["etc/ssh/ssh_host_ed25519_key", "etc/ssh/ssh_host_ed25519_key.pub",
             "etc/ssh/sshd_config", "build/nested/output", "tmp/.hidden", "tmp/temporary",
             "usr/bin/app", "other/etc/ssh/ssh_host_keep", "build-other/keep"]
    for name in files:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    selected = ["/etc/ssh/ssh_host_*", "/build", "/tmp/*"]
    for _ in range(2):
        unroot.run("exclude", "add", str(root), *selected).assert_ok()
    assert rules(unroot, root) == selected
    archive = tmp_path / "root.tar"
    unroot.run("pack", "--single", str(root), str(archive)).assert_ok()
    names = contents(archive)
    assert "etc/ssh/sshd_config" in names
    assert "usr/bin/app" in names
    assert "other/etc/ssh/ssh_host_keep" in names
    assert "build-other/keep" in names
    assert "tmp" in names
    assert not any(name.startswith(("build/", "tmp/", ".unroot")) for name in names)
    assert "build" not in names
    assert not any(name.startswith("etc/ssh/ssh_host_") for name in names)
    assert all((root / name).read_text() == name for name in files)
    unroot.run("exclude", "remove", str(root), "/tmp/*").assert_ok()
    unroot.run("pack", "--single", str(root), str(archive)).assert_ok()
    assert "tmp/.hidden" in contents(archive)
    unroot.run("exclude", "clear", str(root)).assert_ok()
    assert rules(unroot, root) == []
    unroot.run("pack", "--single", str(root), str(archive)).assert_ok()
    assert set(files) <= contents(archive)


@pytest.mark.parametrize("pattern", ["etc/ssh/*", "/", "/../x", "/a/./b", "/a//b", "/a/", "/.unroot", "/.unroot/*", "/**", "/a\nb"])
def test_invalid_exclusions_are_atomic(unroot: UnrootRunner, tmp_path: Path, pattern: str) -> None:
    root = tmp_path / "root"
    root.mkdir()
    result = unroot.run("exclude", "add", str(root), "/valid", pattern)
    assert result.returncode != 0
    assert rules(unroot, root) == []


def test_missing_remove_is_atomic_and_pack_rejects_corrupt_rules(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    root = tmp_path / "root"
    root.mkdir()
    unroot.run("exclude", "add", str(root), "/keep").assert_ok()
    result = unroot.run("exclude", "remove", str(root), "/keep", "/absent")
    assert result.returncode != 0
    assert rules(unroot, root) == ["/keep"]
    (root / ".unroot/exclusions.json").write_text("not JSON")
    archive = tmp_path / "root.tar"
    archive.write_bytes(b"previous")
    result = unroot.run("pack", "--single", "--force", str(root), str(archive))
    assert result.returncode != 0
    assert "malformed exclusion registry" in result.stderr
    assert archive.read_bytes() == b"previous"


def test_exclusion_registry_does_not_follow_host_symlinks(unroot: UnrootRunner, tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (root / ".unroot").symlink_to(outside)
    result = unroot.run("exclude", "add", str(root), "/anything")
    assert result.returncode != 0
    assert not list(outside.iterdir())


def test_exclude_obeys_the_pack_lock(unroot: UnrootRunner, tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = unroot.run("exclude", "add", str(root), "/anything")
        assert result.returncode != 0
        assert "already active" in result.stderr
    finally:
        os.close(descriptor)


def test_default_and_injection_precedence(unroot: UnrootRunner, empty_rootfs: Path, tmp_path: Path) -> None:
    root = empty_rootfs
    assert rules(unroot, root) == ["/etc/ssh/ssh_host_*"]
    (root / "etc").mkdir(exist_ok=True)
    (root / "etc/original").write_text("portable original")
    source = tmp_path / "host-file"
    source.write_text("injected content")
    unroot.run("inject", "add", str(root), f"{source}:/etc/original").assert_ok()
    unroot.run("exclude", "add", str(root), "/etc").assert_ok()
    archive = tmp_path / "root.tar"
    unroot.run("pack", str(root), str(archive)).assert_ok()
    assert not any(name == "etc" or name.startswith("etc/") for name in contents(archive))
    assert (root / "etc/original").read_text() == "injected content"
    unroot.run("exclude", "remove", str(root), "/etc").assert_ok()
    unroot.run("pack", str(root), str(archive)).assert_ok()
    with tarfile.open(archive) as packed:
        member = next(entry for entry in packed if entry.name.removeprefix("./") == "etc/original")
        assert packed.extractfile(member).read() == b"portable original"
    assert (root / "etc/original").read_text() == "injected content"


def test_default_host_key_rule_is_removable_and_not_a_live_deletion(
    unroot: UnrootRunner, empty_rootfs: Path, tmp_path: Path
) -> None:
    root = empty_rootfs
    keys = root / "etc/ssh"
    keys.mkdir(parents=True, exist_ok=True)
    (keys / "ssh_host_example_key").write_text("test fixture, not a real key")
    archive = tmp_path / "root.tar"
    unroot.run("pack", str(root), str(archive)).assert_ok()
    assert "etc/ssh/ssh_host_example_key" not in contents(archive)
    assert (keys / "ssh_host_example_key").is_file()
    unroot.run("exclude", "remove", str(root), "/etc/ssh/ssh_host_*").assert_ok()
    unroot.run("pack", str(root), str(archive)).assert_ok()
    assert "etc/ssh/ssh_host_example_key" in contents(archive)
    assert rules(unroot, root) == []


def test_globs_are_anchored_and_support_question_and_brackets(unroot: UnrootRunner, tmp_path: Path) -> None:
    root = tmp_path / "root"
    for name in ("file1.txt", "file2.txt", "file3.txt", "file11.txt", "sub/file1.txt"):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    unroot.run("exclude", "add", str(root), "/file[12].t?t").assert_ok()
    archive = tmp_path / "root.tar"
    unroot.run("pack", "--single", str(root), str(archive)).assert_ok()
    names = contents(archive)
    assert {"file3.txt", "file11.txt", "sub/file1.txt"} <= names
    assert not {"file1.txt", "file2.txt"}.intersection(names)
