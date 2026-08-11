from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import UnrootRunner, create_rootfs, find_static_busybox, run_command


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def _require_gnu_tar() -> None:
    result = subprocess.run(
        ["tar", "--version"], check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.returncode != 0 or "GNU tar" not in result.stdout:
        pytest.skip("archive actions currently require GNU tar")


def _create_archive(source: Path, archive: Path) -> None:
    subprocess.run(
        [
            "tar", "--create", "--format=pax", "--numeric-owner",
            "--owner=0", "--group=0", "--acls", "--xattrs",
            "--xattrs-include=*", "--sparse", f"--file={archive}",
            "--directory", str(source), ".",
        ],
        check=True,
    )


def _metadata_limited_tar(tmp_path: Path) -> Path:
    tool = tmp_path / "tools" / "tar"
    tool.parent.mkdir()
    tool.write_text(
        """#!/bin/sh
case " $* " in
  *" --wildcards "*) exit 1 ;;
esac

directory=.
member=
probe=false
for argument do
    case "$argument" in
        --directory=*) directory=${argument#--directory=} ;;
        --file=/dev/null) probe=true ;;
        --*) ;;
        *) member=$argument ;;
    esac
done

if "$probe" && [ -n "$member" ] && [ -f "$directory/$member" ]; then
    echo "tar: POSIX ACL support is not available" >&2
fi
exit 0
""",
        encoding="utf-8",
    )
    tool.chmod(0o755)
    return tool


def _blocking_tar(tmp_path: Path) -> tuple[Path, Path, Path]:
    real_tar = shutil.which("tar")
    assert real_tar is not None
    tools = tmp_path / "blocking-tools"
    tools.mkdir()
    started = tmp_path / "tar-started"
    release = tmp_path / "tar-release"
    tool = tools / "tar"
    tool.write_text(
        f"""#!/bin/sh
case " $* " in
  *" --create "*|*" --extract "*)
    case " $* " in
      *" --file=/dev/null "*) ;;
      *)
        : > {started}
        while [ ! -e {release} ]; do read -r _ < /dev/null || :; done
        ;;
    esac
    ;;
esac
exec {real_tar} "$@"
""",
        encoding="utf-8",
    )
    tool.chmod(0o755)
    return tools, started, release


def _start_unroot(
    unroot: UnrootRunner,
    arguments: list[str],
    env: dict[str, str],
) -> subprocess.Popen[str]:
    process_env = dict(os.environ)
    for name in tuple(process_env):
        if name.startswith("UNROOT_"):
            process_env.pop(name)
    process_env.update(env)
    process_env["UNROOT_SUDO"] = str(unroot.sudo_guard)
    process_env["LC_ALL"] = "C"
    return subprocess.Popen(
        [str(unroot.binary), *arguments],
        cwd=unroot.repo,
        env=process_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )


def _wait_for_path(path: Path, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            pytest.fail(
                f"archive owner exited before reaching tar\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        time.sleep(0.01)
    pytest.fail("archive owner did not reach tar")


def _assert_payload_tree(root: Path) -> None:
    payload = root / "payload"
    assert payload.read_text(encoding="utf-8") == "rootfs payload\n"
    assert payload.stat().st_mode & 0o777 == 0o751
    assert int(payload.stat().st_mtime) == 1_700_000_000
    assert os.getxattr(payload, b"user.unroot-test") == b"preserved"
    assert (root / "hard-link").stat().st_ino == payload.stat().st_ino
    assert os.readlink(root / "symbolic-link") == "payload"
    assert (root / "nested" / ".unroot" / "data").read_text(
        encoding="utf-8"
    ) == "portable\n"
    sparse = (root / "sparse").stat()
    assert sparse.st_size == 1024 * 1024 + 1
    assert sparse.st_blocks * 512 < sparse.st_size


def test_pack_and_unpack_round_trip_rootfs_metadata(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    _require_gnu_tar()
    source = tmp_path / "source"
    source.mkdir()
    payload = source / "payload"
    payload.write_text("rootfs payload\n", encoding="utf-8")
    payload.chmod(0o751)
    os.utime(payload, (1_700_000_000, 1_700_000_000))
    os.link(payload, source / "hard-link")
    os.symlink("payload", source / "symbolic-link")
    nested_private = source / "nested" / ".unroot"
    nested_private.mkdir(parents=True)
    (nested_private / "data").write_text("portable\n", encoding="utf-8")
    with (source / "sparse").open("wb") as sparse:
        sparse.seek(1024 * 1024)
        sparse.write(b"x")
    try:
        os.setxattr(payload, b"user.unroot-test", b"preserved")
    except OSError as error:
        pytest.skip(f"test filesystem has no user xattr support: {error}")
    original = tmp_path / "original.tar"
    _create_archive(source, original)

    root = tmp_path / "root"
    result = unroot.run("unpack", str(original), str(root))
    require_capability(
        result.returncode == 0,
        "archive round trip requires rich ID mapping:\n" + result.diagnostic(),
        "rich_idmap",
    )
    result.assert_ok()
    _assert_payload_tree(root)
    metadata = json.loads((root / ".unroot" / "meta.json").read_text())
    assert metadata["version"] == "unroot.meta/v1"
    assert metadata["idmap"]["mode"] == "rich"

    captured = tmp_path / "captured.tar.gz"
    unroot.run("pack", str(root), str(captured)).assert_ok()
    members = subprocess.run(
        ["tar", "--list", f"--file={captured}"], check=True, text=True,
        stdout=subprocess.PIPE,
    ).stdout.splitlines()
    assert "./payload" in members
    assert not any(name == ".unroot" or name.startswith("./.unroot") for name in members)

    restored = tmp_path / "restored"
    unroot.run("unpack", str(captured), str(restored)).assert_ok()
    _assert_payload_tree(restored)


def test_pack_requires_initialized_rootfs(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_gnu_tar()
    root = tmp_path / "plain-root"
    root.mkdir()
    (root / "payload").write_text("data", encoding="utf-8")
    result = unroot.run("pack", str(root), str(tmp_path / "output.tar"))
    assert result.returncode != 0
    assert "has no ID-map metadata" in result.stderr


def test_pack_rejects_tar_that_cannot_preserve_metadata(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    tool = _metadata_limited_tar(tmp_path)
    root = tmp_path / "root"
    root.mkdir()
    archive = tmp_path / "output.tar"
    path = str(tool.parent) + os.pathsep + os.environ.get("PATH", "")

    result = unroot.run("pack", str(root), str(archive), env={"PATH": path})

    assert result.returncode != 0
    assert "POSIX ACL support is not available" in result.stderr
    assert "Use --force to accept metadata loss" in result.stderr
    assert not archive.exists()


def test_failed_pack_does_not_publish_partial_archive(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
) -> None:
    tools = tmp_path / "failing-tools"
    tools.mkdir()
    real_tar = shutil.which("tar")
    assert real_tar is not None
    tool = tools / "tar"
    tool.write_text(
        f"""#!/bin/sh
for argument do
    if [ "$argument" = --file=/dev/null ]; then
        exec {real_tar} "$@"
    fi
done
exit 42
""",
        encoding="utf-8",
    )
    tool.chmod(0o755)
    archive = tmp_path / "partial.tar"
    path = str(tools) + os.pathsep + os.environ.get("PATH", "")

    result = unroot.run(
        "pack", str(managed_rootfs), str(archive), env={"PATH": path}
    )

    assert result.returncode == 42
    assert not archive.exists()
    assert not list(tmp_path.glob(".unroot-archive-*"))


def test_packed_archive_honors_process_umask(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
) -> None:
    archive = tmp_path / "mode.tar"
    previous = os.umask(0o027)
    try:
        result = unroot.run("pack", str(managed_rootfs), str(archive))
    finally:
        os.umask(previous)

    result.assert_ok()
    assert stat.S_IMODE(archive.stat().st_mode) == 0o640


def test_pack_and_unpack_reject_an_active_rootfs_archive_lock(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
) -> None:
    import fcntl

    _require_gnu_tar()
    archive = tmp_path / "input.tar"
    unroot.run("pack", str(managed_rootfs), str(archive)).assert_ok()

    descriptor = os.open(managed_rootfs, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)

        packed = unroot.run(
            "pack", str(managed_rootfs), str(tmp_path / "blocked.tar")
        )
        assert packed.returncode != 0
        assert "archive operation is already active for ROOT" in packed.stderr

        unpacked = unroot.run("unpack", str(archive), str(managed_rootfs))
        assert unpacked.returncode != 0
        assert "archive operation is already active for ROOT" in unpacked.stderr
    finally:
        os.close(descriptor)


def test_unpack_rejects_an_active_lock_before_root_creation(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
) -> None:
    import fcntl

    _require_gnu_tar()
    archive = tmp_path / "input.tar"
    unroot.run("pack", str(managed_rootfs), str(archive)).assert_ok()
    root = tmp_path / "new-root"
    root.mkdir()
    descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)

        result = unroot.run("unpack", str(archive), str(root))

        assert result.returncode != 0
        assert "archive operation is already active for ROOT" in result.stderr
        assert not any(root.iterdir())
    finally:
        os.close(descriptor)


@pytest.mark.parametrize(
    ("owner_action", "contender_action"),
    [
        ("pack", "pack"),
        ("pack", "unpack"),
        ("unpack", "pack"),
        ("unpack", "unpack"),
    ],
)
def test_archive_operation_pairs_share_one_rootfs_lock(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
    owner_action: str,
    contender_action: str,
) -> None:
    _require_gnu_tar()
    source_archive = tmp_path / "source.tar"
    unroot.run("pack", str(managed_rootfs), str(source_archive)).assert_ok()
    root = managed_rootfs
    if owner_action == "unpack":
        root = tmp_path / "unpack-root"

    tools, started, release = _blocking_tar(tmp_path)
    path = str(tools) + os.pathsep + os.environ.get("PATH", "")
    owner_arguments = (
        ["pack", str(root), str(tmp_path / "owner.tar")]
        if owner_action == "pack"
        else ["unpack", str(source_archive), str(root)]
    )
    owner = _start_unroot(unroot, owner_arguments, {"PATH": path})
    try:
        _wait_for_path(started, owner)
        contender = (
            unroot.run(
                "pack",
                str(root),
                str(tmp_path / "contender.tar"),
                env={"PATH": path},
            )
            if contender_action == "pack"
            else unroot.run(
                "unpack",
                str(source_archive),
                str(root),
                env={"PATH": path},
            )
        )
        assert contender.returncode != 0
        assert "archive operation is already active for ROOT" in contender.stderr
    finally:
        release.touch()
        stdout, stderr = owner.communicate(timeout=10)
    assert owner.returncode == 0, f"stdout:\n{stdout}\nstderr:\n{stderr}"


def test_unpack_refuses_to_overlay_an_existing_tree(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_gnu_tar()
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("archive", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"
    root.mkdir()
    existing = root / "existing"
    existing.write_text("keep", encoding="utf-8")

    result = unroot.run("unpack", str(archive), str(root))
    assert result.returncode != 0
    assert "ROOT must be empty" in result.stderr
    assert existing.read_text(encoding="utf-8") == "keep"


def test_unpack_rejects_tar_that_cannot_preserve_metadata(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    tool = _metadata_limited_tar(tmp_path)
    archive = tmp_path / "input.tar"
    archive.touch()
    root = tmp_path / "root"
    path = str(tool.parent) + os.pathsep + os.environ.get("PATH", "")

    result = unroot.run("unpack", str(archive), str(root), env={"PATH": path})

    assert result.returncode != 0
    assert "POSIX ACL support is not available" in result.stderr
    assert "Use --force to accept metadata loss" in result.stderr
    assert not root.exists()


def test_unpack_force_accepts_reported_metadata_loss(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    tool = _metadata_limited_tar(tmp_path)
    archive = tmp_path / "input.tar"
    archive.touch()
    root = tmp_path / "root"
    path = str(tool.parent) + os.pathsep + os.environ.get("PATH", "")

    result = unroot.run(
        "unpack", "--force", str(archive), str(root), env={"PATH": path}
    )

    assert "POSIX ACL support is not available" in result.stderr
    assert "Continuing because --force was specified" in result.stderr
    assert "Use --force to accept metadata loss" not in result.stderr


def test_unpack_rejects_private_unroot_metadata(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_gnu_tar()
    source = tmp_path / "source"
    private = source / ".unroot"
    private.mkdir(parents=True)
    (private / "meta.json").write_text("poison\n", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"

    result = unroot.run("unpack", str(archive), str(root))
    assert result.returncode != 0
    assert "reserved .unroot metadata tree" in result.stderr
    assert not root.exists()


def test_unpack_preflights_rich_mapping_before_creating_root(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_gnu_tar()
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("archive", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    target = tmp_path / "root"
    standalone = tmp_path / "standalone" / "unroot"
    standalone.parent.mkdir()
    shutil.copy2(unroot.binary, standalone)
    isolated = UnrootRunner(standalone, unroot.repo, unroot.sudo_guard)

    result = isolated.run("unpack", str(archive), str(target))

    assert result.returncode != 0, result.diagnostic()
    assert "unroot-util" in result.stderr
    assert "sudo unroot unpack --native" in result.stderr
    assert not target.exists()


def test_unpack_rejects_symlinked_metadata_without_writing_outside_root(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_gnu_tar()
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("archive", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "outside"
    outside.mkdir()
    (root / ".unroot").symlink_to(outside, target_is_directory=True)

    result = unroot.run("unpack", str(archive), str(root))

    assert result.returncode != 0, result.diagnostic()
    assert not (outside / "meta.json").exists()


def test_native_unpack_persists_native_ownership_when_root_is_available(
    unroot: UnrootRunner, tmp_path: Path, privileged_prefix: tuple[str, ...]
) -> None:
    _require_gnu_tar()
    busybox = find_static_busybox()
    if busybox is None:
        pytest.skip("a static BusyBox is required for native entry coverage")
    source = create_rootfs(tmp_path / "source", busybox)
    (source / "payload").write_text("native\n", encoding="utf-8")
    archive = tmp_path / "native.tar"
    _create_archive(source, archive)
    root = tmp_path / "native-root"
    command = [
        *privileged_prefix,
        str(unroot.binary),
        "unpack",
        "--native",
        str(archive),
        str(root),
    ]

    try:
        run_command(command).assert_ok()
        metadata_path = root / ".unroot" / "meta.json"
        content = (
            metadata_path.read_text()
            if os.geteuid() == 0
            else run_command([*privileged_prefix, "cat", str(metadata_path)]).assert_ok().stdout
        )
        metadata = json.loads(content)
        assert metadata["idmap"] == {
            "mode": "native",
            "source": "host",
            "uid_map": [],
            "gid_map": [],
        }
        assert (root / "payload").stat().st_uid == 0
        enter = [
            str(unroot.binary), "enter", str(root), "--",
            "/bin/busybox", "id", "-u",
        ]
        enter = [*privileged_prefix, *enter]
        result = run_command(enter).assert_ok()
        assert result.stdout.strip() == "0"
    finally:
        if root.exists() and os.geteuid() != 0:
            run_command([*privileged_prefix, "rm", "-rf", str(root)])
