from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import UnrootRunner, run_command


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.rich_idmap,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


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

    setting = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/busybox", "cat",
        "/proc/self/setgroups",
    ).assert_ok()
    assert setting.stdout.strip() == "allow"

    changed = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/setgroups-probe", "1"
    ).assert_ok()
    assert changed.stdout.strip() == "group:1"


def test_rich_mapping_translates_namespace_ownership(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    require_rich_idmap,
) -> None:
    count = 65535
    uid_start, gid_start = require_rich_idmap(count)

    owned = managed_rootfs / "tmp" / "rich-owned"
    result = unroot.run(
        "enter", str(managed_rootfs), "--", "/bin/busybox", "sh", "-c",
        "/bin/busybox touch /tmp/rich-owned && "
        "/bin/busybox chown 1:1 /tmp/rich-owned && "
        "/bin/busybox stat -c %u:%g /tmp/rich-owned",
    ).assert_ok()

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
    require_rich_idmap,
) -> None:
    count = 65535
    _, gid_start = require_rich_idmap(count)

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
    require_rich_idmap,
) -> None:
    count = 65535
    uid_start, gid_start = require_rich_idmap(count)
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
    unroot.run("unpack", "--inject=-*", str(archive), str(root)).assert_ok()
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
        "unpack", "--inject=-*", str(captured), str(restored)
    ).assert_ok()
    status = (restored / "owned").stat()
    assert status.st_uid == uid_start
    assert status.st_gid == gid_start + 1


def test_custom_rich_mapping_preserves_large_logical_ids(
    unroot: UnrootRunner,
    managed_rootfs: Path,
    tmp_path: Path,
    require_rich_idmap,
) -> None:
    count = 100000
    uid_start, gid_start = require_rich_idmap(count)
    high_id = tmp_path / "high-id"
    high_id.write_text("mapped\n", encoding="utf-8")
    source_archive = tmp_path / "source.tar"
    unroot.run("pack", str(managed_rootfs), str(source_archive)).assert_ok()
    subprocess.run(
        [
            "tar", "--append", "--format=pax", "--numeric-owner",
            "--owner=100000", "--group=100000", f"--file={source_archive}",
            "--directory", str(tmp_path), high_id.name,
        ],
        check=True,
    )

    root = tmp_path / "custom-root"
    unroot.run(
        "unpack", "--id-count", str(count), str(source_archive), str(root)
    ).assert_ok()
    visible = unroot.run(
        "enter", str(root), "--", "/bin/busybox", "stat", "-c", "%u:%g",
        "/high-id",
    ).assert_ok()
    assert visible.stdout.strip() == "100000:100000"

    status = (root / "high-id").stat()
    assert status.st_uid == uid_start + count - 1
    assert status.st_gid == gid_start + count - 1

    metadata = json.loads(
        (root / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["idmap"]["uid_map"][1]["count"] == count
    assert metadata["idmap"]["gid_map"][1]["count"] == count

    reused = unroot.run(
        "enter", str(root), "--", "/bin/busybox", "stat", "-c", "%u:%g",
        "/high-id",
    ).assert_ok()
    assert reused.stdout.strip() == "100000:100000"

    captured = tmp_path / "captured.tar"
    unroot.run("pack", str(root), str(captured)).assert_ok()
    with tarfile.open(captured) as packed:
        member = packed.getmember("./high-id")
        assert member.uid == 100000
        assert member.gid == 100000
