from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Callable, Optional, Tuple

import pytest

from .support import CommandResult, UnrootRunner, run_command


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.rich_idmap,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def _helper_allocation(
    helper: Path, count: int
) -> Tuple[Optional[Tuple[int, int]], str]:
    if not helper.is_file() or not os.access(helper, os.X_OK):
        return None, f"unroot-util not found or not executable: {helper}"
    result = subprocess.run(
        [str(helper), "idmap", "--count", str(count)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        return None, (result.stderr or result.stdout).strip()
    fields = result.stdout.split()
    if len(fields) != 5 or fields[0] != "unroot-idmap-v1":
        return None, f"invalid unroot-util response: {result.stdout!r}"
    try:
        uid_start, gid_start, returned = map(int, fields[1:4])
    except ValueError:
        return None, f"invalid unroot-util response: {result.stdout!r}"
    if returned != count:
        return None, f"unroot-util returned {returned} IDs, expected {count}"
    return (uid_start, gid_start), ""


def _require_allocation(
    unroot: UnrootRunner,
    count: int,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> Tuple[int, int]:
    allocation, detail = _helper_allocation(
        unroot.binary.with_name("unroot-util"), count
    )
    helpers = (
        shutil.which("newuidmap") is not None
        and shutil.which("newgidmap") is not None
    )
    require_capability(
        helpers and allocation is not None,
        "rich ID mapping requires unroot-util, newuidmap, newgidmap, and a "
        f"subordinate-ID allocation ({detail or 'mapping helpers unavailable'})",
        "rich_idmap",
    )
    assert allocation is not None
    return allocation


def _require_mapping(
    result: CommandResult,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> CommandResult:
    require_capability(
        result.returncode == 0,
        "rich ID mapping is blocked by the host policy:\n" + result.diagnostic(),
        "rich_idmap",
    )
    return result.assert_ok()


def _compile_setgroups_probe(
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> Path:
    compiler = shutil.which(os.environ.get("CC", "cc"))
    require_capability(
        compiler is not None,
        "a C compiler is required for setgroups qualification",
        "rich_idmap",
    )
    assert compiler is not None
    source = Path(__file__).with_name("fixtures") / "setgroups_probe.c"
    probe = tmp_path / "setgroups-probe"
    built = run_command(
        [
            compiler,
            "-static",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-o",
            str(probe),
            str(source),
        ]
    )
    require_capability(
        built.returncode == 0,
        "a static C toolchain is required for setgroups qualification:\n"
        + built.diagnostic(),
        "rich_idmap",
    )
    return probe


def test_rich_mapping_allows_supplementary_group_changes(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    probe = _compile_setgroups_probe(tmp_path, require_capability)
    shutil.copy2(probe, managed_rootfs / "bin" / probe.name)

    setting = _require_mapping(
        unroot.run(
            "enter",
            str(managed_rootfs),
            "--",
            "/bin/busybox",
            "cat",
            "/proc/self/setgroups",
        ),
        require_capability,
    )
    assert setting.stdout.strip() == "allow"

    changed = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/setgroups-probe", "1"
    ).assert_ok()
    assert changed.stdout.strip() == "group:1"


def test_rich_mapping_translates_namespace_ownership(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    count = 65535
    uid_start, gid_start = _require_allocation(
        unroot, count, require_capability
    )

    owned = managed_rootfs / "tmp" / "rich-owned"
    result = _require_mapping(
        unroot.run(
            "enter",
            str(managed_rootfs),
            "--",
            "/bin/busybox",
            "sh",
            "-c",
            "/bin/busybox touch /tmp/rich-owned && "
            "/bin/busybox chown 1:1 /tmp/rich-owned && "
            "/bin/busybox stat -c %u:%g /tmp/rich-owned",
        ),
        require_capability,
    )

    assert result.stdout.strip() == "1:1"
    status = owned.stat()
    assert status.st_uid == uid_start
    assert status.st_gid == gid_start

    metadata = json.loads(
        (managed_rootfs / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["version"] == "unroot.meta/v1"
    assert metadata["idmap"]["uid_map"] == [
        {"inside": 0, "outside": os.getuid(), "count": 1},
        {"inside": 1, "outside": uid_start, "count": count},
    ]
    assert metadata["idmap"]["gid_map"] == [
        {"inside": 0, "outside": os.getgid(), "count": 1},
        {"inside": 1, "outside": gid_start, "count": count},
    ]

    reused = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/busybox", "stat", "-c",
        "%u:%g", "/tmp/rich-owned",
    ).assert_ok()
    assert reused.stdout.strip() == "1:1"

def test_rich_mapping_refuses_stale_recorded_allocation(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    count = 65535
    _, gid_start = _require_allocation(unroot, count, require_capability)

    metadata_path = managed_rootfs / ".unroot" / "meta.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    helper = unroot.binary.parent / "unroot-util"
    stale_start = None
    for candidate in ((1 << 32) - count, 1, 1 << 31):
        if candidate <= os.getuid() < candidate + count:
            continue
        checked = run_command(
            [
                str(helper),
                "idmap",
                "--validate",
                str(candidate),
                str(gid_start),
                str(count),
            ]
        )
        if (
            checked.returncode != 0
            and "recorded subordinate UID range is no longer assigned"
            in checked.stderr
        ):
            stale_start = candidate
            break
    assert stale_start is not None, "unable to find an unassigned UID test range"
    metadata["idmap"]["uid_map"][1]["outside"] = stale_start
    metadata_path.write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    marker = managed_rootfs / "tmp" / "should-not-run"

    result = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/busybox", "touch",
        "/tmp/should-not-run",
    )
    assert result.returncode != 0
    assert "recorded subordinate UID range is no longer assigned" in result.stderr
    assert not marker.exists()


def test_rich_archive_round_trip_preserves_logical_ownership(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    count = 65535
    uid_start, gid_start = _require_allocation(
        unroot, count, require_capability
    )
    source = tmp_path / "source"
    source.mkdir()
    (source / "owned").write_text("mapped\n", encoding="utf-8")
    archive = tmp_path / "input.tar"
    subprocess.run(
        [
            "tar", "--create", "--format=pax", "--numeric-owner",
            "--owner=1", "--group=2", f"--file={archive}",
            "--directory", str(source), ".",
        ],
        check=True,
    )

    root = tmp_path / "root"
    _require_mapping(
        unroot.run(
            "unpack", str(archive), str(root)
        ),
        require_capability,
    )
    status = (root / "owned").stat()
    assert status.st_uid == uid_start
    assert status.st_gid == gid_start + 1

    captured = tmp_path / "captured.tar"
    unroot.run("pack", str(root), str(captured)).assert_ok()
    with tarfile.open(captured) as packed:
        member = packed.getmember("./owned")
        assert member.uid == 1
        assert member.gid == 2
    restored = tmp_path / "restored"
    unroot.run(
        "unpack", str(captured), str(restored)
    ).assert_ok()
    status = (restored / "owned").stat()
    assert status.st_uid == uid_start
    assert status.st_gid == gid_start + 1
