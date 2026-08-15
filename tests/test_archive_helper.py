import json
import os
import select
import subprocess
import tarfile
from pathlib import Path
from typing import Optional

import pytest


pytestmark = pytest.mark.skipif(
    os.name != "posix", reason="unroot-util descriptor tests require POSIX"
)


def _run_helper(
    helper: Path, arguments: list[str], descriptor: int, cwd: Path
) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    return subprocess.run(
        [
            str(helper), "archive", arguments[0], "--fd", str(descriptor),
            *arguments[1:],
        ],
        cwd=cwd,
        env=environment,
        pass_fds=(descriptor,),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _inspect_filesystem(
    helper: Path, directory: Path
) -> subprocess.CompletedProcess[str]:
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        return subprocess.run(
            [str(helper), "filesystem", "inspect", "--fd", str(descriptor)],
            pass_fds=(descriptor,),
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    finally:
        os.close(descriptor)


def _run_with_terminal(
    command: list[str],
    cwd: Path,
    pass_fds: tuple[int, ...] = (),
    progress_option: bool = False,
    progress_style: str = "ascii-color",
    environment: Optional[dict[str, str]] = None,
    columns: int = 80,
) -> tuple[subprocess.CompletedProcess[str], str]:
    import fcntl
    import pty
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(
        slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0)
    )
    if progress_option:
        command = [
            *command,
            "--progress-fd",
            str(slave),
            "--progress-style",
            progress_style,
        ]
    child_environment = {**os.environ, "LC_ALL": "C"}
    if environment:
        child_environment.update(environment)
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=child_environment,
        pass_fds=(*pass_fds, slave),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE if progress_option else slave,
    )
    os.close(slave)
    progress = bytearray()
    while process.poll() is None:
        readable, _, _ = select.select([master], [], [], 0.1)
        if readable:
            try:
                progress.extend(os.read(master, 4096))
            except OSError:
                break
    stdout, stderr = process.communicate()
    while True:
        try:
            data = os.read(master, 4096)
        except OSError:
            break
        if not data:
            break
        progress.extend(data)
    os.close(master)
    result = subprocess.CompletedProcess(
        command, process.returncode, stdout, stderr or ""
    )
    return result, progress.decode(errors="replace")


def _run_helper_with_progress(
    helper: Path,
    arguments: list[str],
    descriptor: int,
    cwd: Path,
    progress_style: str = "ascii-color",
    environment: Optional[dict[str, str]] = None,
    columns: int = 80,
) -> tuple[subprocess.CompletedProcess[str], str]:
    command = [
        str(helper),
        "archive",
        arguments[0],
        "--fd",
        str(descriptor),
        *arguments[1:],
    ]
    return _run_with_terminal(
        command,
        cwd,
        pass_fds=(descriptor,),
        progress_option=True,
        progress_style=progress_style,
        environment=environment,
        columns=columns,
    )


def test_filesystem_helper_verifies_archive_metadata_support(tmp_path: Path) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    assert helper.is_file(), "build unroot-util before running helper tests"

    result = _inspect_filesystem(helper, tmp_path)

    assert result.returncode == 0, result.stderr
    protocol, report = result.stdout.split(" ", 1)
    assert protocol == "unroot-filesystem-v1"
    contents = json.loads(report)
    assert contents["posix_acl"] == {"detail": "", "supported": True}
    assert contents["xattr"] == {"detail": "", "supported": True}
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize(
    ("filter_name", "archive_name", "expected_filters"),
    [
        ("none", "output.tar", []),
        ("xz", "output.tar.xz", ["xz"]),
    ],
)
def test_archive_helper_packs_sparse_tree(
    tmp_path: Path,
    filter_name: str,
    archive_name: str,
    expected_filters: list[str],
) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    assert helper.is_file(), "build unroot-util before running helper tests"

    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("payload\n", encoding="utf-8")
    (source / "NetLock_Arany_Fotanúsítvány").write_text(
        "utf-8 pathname\n", encoding="utf-8"
    )
    with (source / "sparse").open("wb") as sparse:
        sparse.seek(1024 * 1024)
        sparse.write(b"x")

    archive = tmp_path / archive_name
    with archive.open("wb") as output:
        packed = _run_helper(
            helper,
            ["pack", "--filter", filter_name],
            output.fileno(),
            source,
        )
    assert packed.returncode == 0, packed.stderr

    with archive.open("rb") as input_file:
        inspected = _run_helper(
            helper, ["inspect"], input_file.fileno(), root
        )
    assert inspected.returncode == 0, inspected.stderr
    protocol, report = inspected.stdout.split(" ", 1)
    assert protocol == "unroot-archive-v1"
    contents = json.loads(report)
    assert contents["filters"] == expected_filters
    assert contents["members"] == 4
    assert contents["metadata"]["sparse_files"]["count"] == 1


def test_archive_helper_reports_interactive_progress(tmp_path: Path) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    assert helper.is_file(), "build unroot-util before running helper tests"

    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_bytes(bytes(range(256)) * 16384)
    archive = tmp_path / "rootfs.tar"
    with archive.open("wb") as output:
        packed, pack_progress = _run_helper_with_progress(
            helper, ["pack", "--filter", "none"], output.fileno(), source
        )
    assert packed.returncode == 0, packed.stderr
    assert "Packing rootfs" in pack_progress
    assert "done" in pack_progress
    assert " in " in pack_progress
    assert "MiB/s" in pack_progress
    assert "=" in pack_progress
    assert "\x1b[36m" in pack_progress

    with archive.open("rb") as input_file:
        inspected, inspect_progress = _run_helper_with_progress(
            helper, ["inspect"], input_file.fileno(), root
        )
    assert inspected.returncode == 0, inspected.stderr
    assert inspected.stdout.startswith("unroot-archive-v1 ")
    assert "Inspecting archive" in inspect_progress
    assert "done" in inspect_progress
    assert "MiB/s" in inspect_progress

    destination = tmp_path / "destination"
    destination.mkdir()
    with archive.open("rb") as input_file:
        unpacked, unpack_progress = _run_helper_with_progress(
            helper, ["unpack"], input_file.fileno(), destination
        )
    assert unpacked.returncode == 0, unpacked.stderr
    assert "Extracting rootfs" in unpack_progress
    assert "done" in unpack_progress
    assert "MiB/s" in unpack_progress
    assert (destination / "payload").read_bytes() == (
        source / "payload"
    ).read_bytes()


def test_archive_progress_style_survives_sanitized_helper_locale(
    tmp_path: Path,
) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_bytes(bytes(range(256)) * 16384)
    archive = tmp_path / "rootfs.tar"

    with tarfile.open(archive, "w") as output:
        output.add(source / "payload", arcname="payload")

    destination = tmp_path / "destination"
    destination.mkdir()
    with archive.open("rb") as input_file:
        unpacked, progress = _run_helper_with_progress(
            helper,
            ["unpack"],
            input_file.fileno(),
            destination,
            progress_style="unicode",
            environment={"LC_ALL": "C"},
        )

    assert unpacked.returncode == 0, unpacked.stderr
    assert "░" in progress
    assert "\x1b[36m" not in progress
    assert (destination / "payload").read_bytes() == (
        source / "payload"
    ).read_bytes()


def test_archive_pack_progress_animates_during_compression(
    tmp_path: Path,
) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_bytes(os.urandom(4 * 1024 * 1024))
    archive = tmp_path / "rootfs.tar.xz"

    with archive.open("wb") as output:
        packed, progress = _run_helper_with_progress(
            helper,
            ["pack", "--filter", "xz"],
            output.fileno(),
            source,
            progress_style="unicode",
        )

    assert packed.returncode == 0, packed.stderr
    frames = progress.split("\r")
    assert sum("Packing rootfs" in frame for frame in frames) >= 4


def test_archive_progress_compacts_for_narrow_terminals(tmp_path: Path) -> None:
    import re

    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_bytes(bytes(range(256)) * 16384)
    archive = tmp_path / "rootfs.tar"

    with archive.open("wb") as output:
        packed, progress = _run_helper_with_progress(
            helper,
            ["pack", "--filter", "none"],
            output.fileno(),
            source,
            environment={"NO_COLOR": "1"},
            columns=36,
        )

    assert packed.returncode == 0, packed.stderr
    frames = progress.replace("\n", "\r").split("\r")
    visible = [re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", frame) for frame in frames]
    assert all(len(frame) <= 36 for frame in visible)


def test_unroot_enables_progress_only_for_terminal_output(tmp_path: Path) -> None:
    root = Path(__file__).parents[1]
    unroot = root / "bin" / "unroot"
    assert unroot.is_file(), "build unroot before running archive progress tests"

    archive = tmp_path / "rootfs.tar"
    with archive.open("wb") as output:
        with tarfile.open(fileobj=output, mode="w") as tar:
            payload = tmp_path / "payload"
            payload.write_text("payload\n", encoding="utf-8")
            tar.add(payload, arcname="payload")

    piped = subprocess.run(
        [str(unroot), "inspect", "archive", str(archive), "--json"],
        cwd=root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert piped.returncode == 0, piped.stderr
    assert "Inspecting archive" not in piped.stderr

    interactive, progress = _run_with_terminal(
        [str(unroot), "inspect", "archive", str(archive), "--json"],
        root,
        environment={"LC_ALL": "C.UTF-8", "NO_COLOR": "1"},
    )
    assert interactive.returncode == 0
    assert json.loads(interactive.stdout)["members"] == 1
    assert "Inspecting archive" in progress
    assert "done" in progress
    assert "░" in progress
    assert "\x1b[36m" not in progress
