from __future__ import annotations

import json
import os
import shutil
import stat
import sys
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import UnrootRunner, run_command


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.rootfs_journey,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


@pytest.mark.parametrize("role", ["NATIVE", "FOREIGN"], ids=["native", "foreign"])
def test_alpine_rootfs_can_be_unpacked_entered_modified_packed_and_restored(
    role: str,
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    archive_value = os.environ.get(f"UNROOT_E2E_ALPINE_{role}", "")
    expected_arch = os.environ.get(f"UNROOT_E2E_ALPINE_{role}_ARCH", "")
    archive = Path(archive_value) if archive_value else Path()
    require_capability(
        bool(archive_value) and archive.is_file() and bool(expected_arch),
        f"{role.lower()} rootfs archive and architecture are not configured",
        "rootfs_journey",
    )
    helper = unroot.binary.with_name("unroot-util")
    allocation = (
        run_command([str(helper), "idmap", "--count", "65535"])
        if helper.is_file() and os.access(helper, os.X_OK)
        else None
    )
    require_capability(
        shutil.which("newuidmap") is not None
        and shutil.which("newgidmap") is not None
        and allocation is not None
        and allocation.returncode == 0,
        "the real-rootfs journey requires rich ID mapping",
        "rootfs_journey",
    )

    root = tmp_path / "root"
    unroot.run("unpack", str(archive), str(root), timeout=90).assert_ok()
    metadata = json.loads(
        (root / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["idmap"]["mode"] == "rich"

    marker = root / "root" / "unroot-journey"
    entered = unroot.run(
        "enter",
        str(root),
        "--",
        "/bin/sh",
        "-c",
        "set -eu; "
        "test -r /proc/self/status; "
        "printf 'created through unroot\\n' > /root/unroot-journey; "
        "chmod 0640 /root/unroot-journey; "
        "/sbin/apk --print-arch; "
        "/bin/busybox stat -c %u:%g /etc/shadow",
        timeout=90,
    ).assert_ok()
    assert entered.stdout.splitlines() == [expected_arch, "0:42"]
    assert marker.read_text(encoding="utf-8") == "created through unroot\n"
    assert stat.S_IMODE(marker.stat().st_mode) == 0o640

    captured = tmp_path / "captured.tar.gz"
    unroot.run("pack", str(root), str(captured), timeout=90).assert_ok()

    restored = tmp_path / "restored"
    unroot.run("unpack", str(captured), str(restored), timeout=90).assert_ok()
    restored_marker = restored / "root" / "unroot-journey"
    assert restored_marker.read_text(encoding="utf-8") == (
        "created through unroot\n"
    )
    assert stat.S_IMODE(restored_marker.stat().st_mode) == 0o640

    verified = unroot.run(
        "enter",
        str(restored),
        "--",
        "/bin/sh",
        "-c",
        "set -eu; "
        "test \"$(cat /root/unroot-journey)\" = 'created through unroot'; "
        "/sbin/apk --print-arch; "
        "/bin/busybox stat -c %u:%g /etc/shadow",
        timeout=90,
    ).assert_ok()
    assert verified.stdout.splitlines() == [expected_arch, "0:42"]
