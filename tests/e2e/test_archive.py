from __future__ import annotations

import io
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import (
    CommandResult,
    UnrootRunner,
    create_rootfs,
    find_static_busybox,
    run_command,
)


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def _require_fixture_tar() -> None:
    result = subprocess.run(
        ["tar", "--version"], check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if result.returncode != 0 or "GNU tar" not in result.stdout:
        pytest.skip("GNU tar is required to create this test fixture")


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


def _create_named_archive(archive: Path, name: str, content: bytes) -> None:
    with tarfile.open(archive, "w", format=tarfile.PAX_FORMAT) as output:
        member = tarfile.TarInfo(name)
        member.size = len(content)
        output.addfile(member, io.BytesIO(content))


def _create_hardlink_archive(archive: Path, name: str, target: str) -> None:
    with tarfile.open(archive, "w", format=tarfile.PAX_FORMAT) as output:
        member = tarfile.TarInfo(name)
        member.type = tarfile.LNKTYPE
        member.linkname = target
        output.addfile(member)


def test_inspect_archive_reports_structured_contents(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    archive = tmp_path / "input.tar"
    _create_named_archive(archive, "nested/payload", b"content\n")

    result = unroot.run("inspect", "archive", str(archive), "--json").assert_ok()
    report = json.loads(result.stdout)

    assert report["members"] == 1
    assert report["regular_bytes"] == 8
    assert report["layout"] == {"oci": False, "reserved_metadata": False}
    assert report["metadata"]["unsafe_paths"]["count"] == 0


def test_unpack_rejects_oci_layout_before_creating_root(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    archive = tmp_path / "oci.tar"
    _create_named_archive(archive, "oci-layout", b'{"imageLayoutVersion":"1.0.0"}\n')
    root = tmp_path / "root"

    result = _run_unroot(
        unroot, ["unpack", "--native", str(archive), str(root)], {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "archive is an OCI image layout" in result.stderr
    assert not root.exists()


def test_nested_oci_layout_name_remains_a_raw_rootfs_member(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    archive = tmp_path / "raw.tar"
    _create_named_archive(archive, "nested/oci-layout", b"ordinary file\n")

    result = unroot.run("inspect", "archive", str(archive), "--json").assert_ok()

    assert json.loads(result.stdout)["layout"]["oci"] is False


@pytest.mark.parametrize("member", ["../outside", "a/../../outside", "/outside"])
def test_unpack_rejects_member_paths_outside_the_rootfs(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
    member: str,
) -> None:
    archive = tmp_path / "unsafe.tar"
    _create_named_archive(archive, member, b"unsafe\n")
    root = tmp_path / "root"

    result = _run_unroot(
        unroot, ["unpack", "--native", str(archive), str(root)], {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "paths outside the rootfs" in result.stderr
    assert not root.exists()


def test_unpack_rejects_hardlinks_to_reserved_metadata(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    archive = tmp_path / "reserved-hardlink.tar"
    _create_hardlink_archive(archive, "payload", ".unroot/meta.json")
    root = tmp_path / "root"

    result = _run_unroot(
        unroot, ["unpack", "--native", str(archive), str(root)], {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "reserved .unroot metadata tree" in result.stderr
    assert not root.exists()


def _isolated_unroot(
    unroot: UnrootRunner, tmp_path: Path, helper_body: str
) -> UnrootRunner:
    directory = tmp_path / "isolated-bin"
    directory.mkdir()
    binary = directory / "unroot"
    helper = directory / "unroot-util"
    real_helper = directory / "unroot-util.real"
    shutil.copy2(unroot.binary, binary)
    shutil.copy2(unroot.binary.parent / "unroot-util", real_helper)
    helper.write_text(
        "#!/bin/sh\n"
        f"real={shlex.quote(str(real_helper))}\n"
        f"{helper_body}\n"
        'exec "$real" "$@"\n',
        encoding="utf-8",
    )
    helper.chmod(0o755)
    return UnrootRunner(binary, unroot.repo, unroot.sudo_guard)


def _blocking_helper(
    unroot: UnrootRunner, tmp_path: Path
) -> tuple[UnrootRunner, Path, Path]:
    started = tmp_path / "archive-started"
    release = tmp_path / "archive-release"
    body = f"""
if [ "$1" = archive ] && {{ [ "$2" = pack ] || [ "$2" = unpack ]; }}; then
    : > {shlex.quote(str(started))}
    while [ ! -e {shlex.quote(str(release))} ]; do
        read -r _ < /dev/null || :
    done
fi
"""
    return _isolated_unroot(unroot, tmp_path, body), started, release


def test_inspect_host_reports_an_incompatible_archive_helper(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    isolated = _isolated_unroot(
        unroot,
        tmp_path,
        'if [ "$1" = archive ] && [ "$2" = --version ]; then '
        'printf "other-v1 archive\\n"; exit 0; fi',
    )

    report = json.loads(isolated.run("inspect", "host", "--json").assert_ok().stdout)

    archive = report["archives"]["libarchive"]
    assert archive["status"] == "unknown"
    assert "incompatible" in archive["detail"]


def test_inspect_archive_rejects_a_malformed_helper_record(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    archive = tmp_path / "input.tar"
    _create_named_archive(archive, "payload", b"content\n")
    isolated = _isolated_unroot(
        unroot,
        tmp_path,
        'if [ "$1" = archive ] && [ "$2" = inspect ]; then '
        'printf "other-v1 {}\\n"; exit 0; fi',
    )

    result = isolated.run("inspect", "archive", str(archive))

    assert result.returncode != 0
    assert "unknown archive inspection protocol" in result.stderr


def _start_unroot(
    unroot: UnrootRunner,
    arguments: list[str],
    env: dict[str, str],
    prefix: tuple[str, ...] = (),
) -> subprocess.Popen[str]:
    process_env = dict(os.environ)
    for name in tuple(process_env):
        if name.startswith("UNROOT_"):
            process_env.pop(name)
    process_env.update(env)
    process_env["UNROOT_SUDO"] = str(unroot.sudo_guard)
    process_env["LC_ALL"] = "C"
    command = [*prefix]
    if prefix:
        command.extend(
            ["/usr/bin/env", *[f"{name}={value}" for name, value in env.items()]]
        )
    command.extend([str(unroot.binary), *arguments])
    return subprocess.Popen(
        command,
        cwd=unroot.repo,
        env=process_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )


def _run_unroot(
    unroot: UnrootRunner,
    arguments: list[str],
    env: dict[str, str],
    prefix: tuple[str, ...] = (),
) -> CommandResult:
    process = _start_unroot(unroot, arguments, env, prefix)
    stdout, stderr = process.communicate(timeout=30)
    return CommandResult(tuple(process.args), process.returncode, stdout, stderr)


def _wait_for_path(path: Path, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process.poll() is not None:
            stdout, stderr = process.communicate()
            pytest.fail(
                f"archive owner exited before reaching the archive helper\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        time.sleep(0.01)
    pytest.fail("archive owner did not reach the archive helper")


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
    _require_fixture_tar()
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
    result = unroot.run("unpack", "--inject=-*", str(original), str(root))
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
    unroot.run(
        "unpack", "--inject=-*", str(captured), str(restored)
    ).assert_ok()
    _assert_payload_tree(restored)


def test_pack_restores_portable_network_configuration(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    busybox = find_static_busybox()
    if busybox is None:
        pytest.skip("a static BusyBox is required for native entry coverage")
    source = create_rootfs(tmp_path / "source", busybox)
    (source / "run").mkdir()
    (source / "run" / "resolv.conf").write_text(
        "nameserver 192.0.2.1\n", encoding="utf-8"
    )
    (source / "etc" / "resolv.conf").symlink_to("../run/resolv.conf")
    input_archive = tmp_path / "input.tar"
    _create_archive(source, input_archive)
    root = tmp_path / "root"
    _run_unroot(
        unroot,
        ["unpack", "--native", str(input_archive), str(root)],
        {},
        privileged_prefix,
    ).assert_ok()
    assert (root / "etc" / "resolv.conf").read_text(
        encoding="utf-8"
    ) == Path("/etc/resolv.conf").read_text(encoding="utf-8")
    archive = tmp_path / "network-config.tar"

    _run_unroot(
        unroot, ["pack", str(root), str(archive)], {}, privileged_prefix
    ).assert_ok()

    with tarfile.open(archive) as packed:
        members = {member.name.removeprefix("./"): member for member in packed}
        assert "etc/resolv.conf" in members
        assert "etc/hosts" not in members
        assert members["etc/resolv.conf"].issym()
        assert members["etc/resolv.conf"].linkname == "../run/resolv.conf"
        resolver = packed.extractfile(members["run/resolv.conf"])
        assert resolver is not None
        assert resolver.read() == b"nameserver 192.0.2.1\n"


def test_native_managed_root_supports_durable_custom_injection(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    busybox = find_static_busybox()
    if busybox is None:
        pytest.skip("a static BusyBox is required for native entry coverage")
    source_root = create_rootfs(tmp_path / "source-root", busybox)
    portable = source_root / "tmp" / "portable-config"
    portable.write_text("portable\n", encoding="utf-8")
    destination = source_root / "etc" / "custom.conf"
    destination.symlink_to("../tmp/portable-config")
    input_archive = tmp_path / "input.tar"
    _create_archive(source_root, input_archive)
    root = tmp_path / "root"
    _run_unroot(
        unroot,
        ["unpack", "--native", str(input_archive), str(root)],
        {},
        privileged_prefix,
    ).assert_ok()
    host_source = tmp_path / "host-config"
    host_source.write_text("injected\n", encoding="utf-8")
    host_link = tmp_path / "host-link"
    host_link.symlink_to(host_source.name)
    spec = f"{host_link}:/etc/custom.conf:1:1:0600"

    _run_unroot(
        unroot,
        ["inject", "add", str(root), spec],
        {},
        privileged_prefix,
    ).assert_ok()
    result = _run_unroot(
        unroot,
        [
            "enter",
            "--native",
            str(root),
            "--",
            "/bin/busybox",
            "sh",
            "-c",
            "stat -c %u:%g:%a /etc/custom.conf; cat /etc/custom.conf",
        ],
        {},
        privileged_prefix,
    ).assert_ok()
    assert result.stdout == "1:1:600\ninjected\n"

    archive = tmp_path / "custom-injection.tar"
    _run_unroot(
        unroot, ["pack", str(root), str(archive)], {}, privileged_prefix
    ).assert_ok()
    with tarfile.open(archive) as packed:
        members = {member.name.removeprefix("./"): member for member in packed}
        assert members["etc/custom.conf"].issym()
        assert members["etc/custom.conf"].linkname == "../tmp/portable-config"

    _run_unroot(
        unroot,
        ["inject", "remove", str(root), "/etc/custom.conf"],
        {},
        privileged_prefix,
    ).assert_ok()
    restored = root / "etc" / "custom.conf"
    assert restored.is_symlink()
    assert restored.readlink() == Path("../tmp/portable-config")


def test_native_injection_rejects_fifo_source_without_blocking(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    source_root = create_rootfs(tmp_path / "source-root")
    input_archive = tmp_path / "input.tar"
    _create_archive(source_root, input_archive)
    root = tmp_path / "root"
    _run_unroot(
        unroot,
        ["unpack", "--native", str(input_archive), str(root)],
        {},
        privileged_prefix,
    ).assert_ok()
    source = tmp_path / "source.fifo"
    os.mkfifo(source)

    result = _run_unroot(
        unroot,
        ["inject", "add", str(root), f"{source}:/etc/custom.conf"],
        {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "does not resolve to a regular file" in result.stderr


def test_inject_clear_preflights_every_preserved_original(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    source_root = create_rootfs(tmp_path / "source-root")
    input_archive = tmp_path / "input.tar"
    _create_archive(source_root, input_archive)
    root = tmp_path / "root"
    _run_unroot(
        unroot,
        ["unpack", "--native", str(input_archive), str(root)],
        {},
        privileged_prefix,
    ).assert_ok()
    resolver_marker = (
        root / ".unroot" / "injections" / "absent" / "etc" / "resolv.conf"
    )
    subprocess.run(
        [*privileged_prefix, "rm", "--", str(resolver_marker)], check=True
    )

    result = _run_unroot(
        unroot,
        ["inject", "clear", str(root)],
        {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "preserved original is missing for /etc/resolv.conf" in result.stderr
    assert (root / "etc" / "hosts").read_text(
        encoding="utf-8"
    ) == Path("/etc/hosts").read_text(encoding="utf-8")


def test_unpack_can_disable_default_injections(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    source = create_rootfs(tmp_path / "source")
    (source / "etc" / "hosts").write_text(
        "portable hosts\n", encoding="utf-8"
    )
    (source / "etc" / "resolv.conf").write_text(
        "portable resolver\n", encoding="utf-8"
    )
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"

    _run_unroot(
        unroot,
        ["unpack", "--native", "--inject=-*", str(archive), str(root)],
        {},
        privileged_prefix,
    ).assert_ok()

    assert (root / "etc" / "hosts").read_text(
        encoding="utf-8"
    ) == "portable hosts\n"
    assert (root / "etc" / "resolv.conf").read_text(
        encoding="utf-8"
    ) == "portable resolver\n"
    listed = _run_unroot(
        unroot,
        ["inject", "list", str(root), "--json"],
        {},
        privileged_prefix,
    ).assert_ok()
    assert json.loads(listed.stdout)["entries"] == []


def test_pack_requires_initialized_rootfs(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_fixture_tar()
    root = tmp_path / "plain-root"
    root.mkdir()
    (root / "payload").write_text("data", encoding="utf-8")
    result = unroot.run("pack", str(root), str(tmp_path / "output.tar"))
    assert result.returncode != 0
    assert "has no ID-map metadata" in result.stderr


def test_pack_does_not_require_tar_on_path(
    unroot: UnrootRunner, managed_rootfs: Path, tmp_path: Path
) -> None:
    empty_path = tmp_path / "empty-path"
    empty_path.mkdir()
    archive = tmp_path / "output.tar"

    result = unroot.run(
        "pack", str(managed_rootfs), str(archive),
        env={"PATH": str(empty_path)},
    )

    result.assert_ok()
    assert archive.is_file()


def test_failed_pack_does_not_publish_partial_archive(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
) -> None:
    isolated = _isolated_unroot(
        unroot, tmp_path,
        'if [ "$1" = archive ] && [ "$2" = pack ]; then exit 42; fi',
    )
    archive = tmp_path / "partial.tar"

    result = isolated.run("pack", str(managed_rootfs), str(archive))

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
    source_archive = tmp_path / "source.tar"
    unroot.run("pack", str(managed_rootfs), str(source_archive)).assert_ok()
    root = managed_rootfs
    if owner_action == "unpack":
        root = tmp_path / "unpack-root"

    isolated, started, release = _blocking_helper(unroot, tmp_path)
    owner_arguments = (
        ["pack", str(root), str(tmp_path / "owner.tar")]
        if owner_action == "pack"
        else ["unpack", str(source_archive), str(root)]
    )
    owner = _start_unroot(isolated, owner_arguments, {})
    try:
        _wait_for_path(started, owner)
        contender = (
            isolated.run(
                "pack",
                str(root),
                str(tmp_path / "contender.tar"),
            )
            if contender_action == "pack"
            else isolated.run(
                "unpack",
                str(source_archive),
                str(root),
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
    _require_fixture_tar()
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


@pytest.mark.parametrize("force", [False, True])
def test_unpack_does_not_require_tar_on_path(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
    force: bool,
) -> None:
    _require_fixture_tar()
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("archive\n", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"
    empty_path = tmp_path / "empty-path"
    empty_path.mkdir()
    arguments = ["unpack", "--native", "--inject=-*"]
    if force:
        arguments.append("--force")
    arguments.extend([str(archive), str(root)])

    result = _run_unroot(
        unroot, arguments, {"PATH": str(empty_path)}, privileged_prefix
    )

    result.assert_ok()
    assert (root / "payload").read_text(encoding="utf-8") == "archive\n"


def test_unpack_rejects_private_unroot_metadata(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    source = tmp_path / "source"
    private = source / ".unroot"
    private.mkdir(parents=True)
    (private / "meta.json").write_text("poison\n", encoding="utf-8")
    archive = tmp_path / "input.tar"
    _create_archive(source, archive)
    root = tmp_path / "root"

    result = _run_unroot(
        unroot,
        ["unpack", "--native", str(archive), str(root)],
        {},
        privileged_prefix,
    )
    assert result.returncode != 0
    assert "reserved .unroot metadata tree" in result.stderr
    assert not root.exists()


@pytest.mark.parametrize(
    "member",
    [
        ".unroot/meta.json",
        "./.unroot/meta.json",
        ".//.unroot/meta.json",
        "././.unroot/meta.json",
        "/.unroot/meta.json",
        "./" * 100 + ".unroot/meta.json",
    ],
)
def test_unpack_rejects_normalized_private_metadata_names(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
    member: str,
) -> None:
    archive = tmp_path / "input.tar"
    _create_named_archive(archive, member, b'{"poison": true}\n')
    root = tmp_path / "root"

    result = _run_unroot(
        unroot,
        ["unpack", "--native", str(archive), str(root)],
        {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    assert "reserved .unroot metadata tree" in result.stderr
    assert not root.exists()


def test_unpack_rejects_reserved_metadata_before_extraction(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    archive = tmp_path / "input.tar"
    _create_named_archive(archive, ".//.unroot/meta.json", b"poison\n")
    log = tmp_path / "archive-operations"
    isolated = _isolated_unroot(
        unroot, tmp_path,
        f'printf "%s %s\\n" "$1" "$2" >> {shlex.quote(str(log))}',
    )

    result = _run_unroot(
        isolated,
        ["unpack", "--native", str(archive), str(tmp_path / "root")],
        {},
        privileged_prefix,
    )

    assert result.returncode != 0, result.diagnostic()
    operations = log.read_text(encoding="utf-8").splitlines()
    assert "archive inspect" in operations
    assert "archive unpack" not in operations


def test_unpack_uses_the_archive_opened_before_validation(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    _require_fixture_tar()
    original = tmp_path / "original"
    original.mkdir()
    (original / "payload").write_text("original\n", encoding="utf-8")
    replacement = tmp_path / "replacement"
    replacement.mkdir()
    (replacement / "payload").write_text("replacement\n", encoding="utf-8")
    archive = tmp_path / "input.tar"
    alternate = tmp_path / "alternate.tar"
    _create_archive(original, archive)
    _create_archive(replacement, alternate)

    started = tmp_path / "extract-started"
    release = tmp_path / "extract-release"
    isolated = _isolated_unroot(
        unroot, tmp_path,
        f"""
if [ "$1" = archive ] && [ "$2" = unpack ]; then
    : > {shlex.quote(str(started))}
    while [ ! -e {shlex.quote(str(release))} ]; do
        read -r _ < /dev/null || :
    done
fi
""",
    )
    root = tmp_path / "root"
    process = _start_unroot(
        isolated,
        ["unpack", "--native", "--inject=-*", str(archive), str(root)],
        {},
        privileged_prefix,
    )
    try:
        _wait_for_path(started, process)
        os.replace(alternate, archive)
        release.touch()
        stdout, stderr = process.communicate(timeout=10)
    finally:
        release.touch()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)

    try:
        assert process.returncode == 0, f"stdout:\n{stdout}\nstderr:\n{stderr}"
        assert (root / "payload").read_text(encoding="utf-8") == "original\n"
    finally:
        run_command([*privileged_prefix, "rm", "-rf", str(root)]).assert_ok()


def test_unpack_requires_the_sibling_archive_helper_before_creating_root(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_fixture_tar()
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
    assert not target.exists()


def test_unpack_rejects_symlinked_metadata_without_writing_outside_root(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    _require_fixture_tar()
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
    _require_fixture_tar()
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
