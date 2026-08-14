from __future__ import annotations

import json
import os
import stat
import sys
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import UnrootRunner, rich_idmap_available


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.rootfs_journey,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def test_funtoo_stage_can_be_entered_built_packed_and_restored(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    archive_value = os.environ.get("UNROOT_E2E_FUNTOO", "")
    expected_chost = os.environ.get("UNROOT_E2E_FUNTOO_CHOST", "")
    stage_name = os.environ.get("UNROOT_E2E_FUNTOO_NAME", "Funtoo")
    archive = Path(archive_value) if archive_value else Path()
    require_capability(
        bool(archive_value) and archive.is_file() and bool(expected_chost),
        f"{stage_name} stage3 archive and CHOST are not configured",
        "rootfs_journey",
    )
    require_capability(
        rich_idmap_available(unroot),
        "the Funtoo rootfs journey requires rich ID mapping",
        "rootfs_journey",
    )

    root = tmp_path / "root"
    unroot.run("unpack", str(archive), str(root), timeout=300).assert_ok()
    metadata = json.loads(
        (root / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["idmap"]["mode"] == "rich"

    marker = root / "root" / "unroot-heritage"
    binary = root / "root" / "unroot-qualification"
    entered = unroot.run(
        "enter",
        str(root),
        "--",
        "/bin/bash",
        "-c",
        "set -eu; "
        "test -r /proc/self/status; "
        "test -c /dev/null; "
        "test -c /dev/ptmx; "
        "test -d /dev/pts; "
        "test -s /etc/resolv.conf; "
        "/usr/bin/python3 -c 'import os; p = os.openpty(); "
        "os.close(p[0]); os.close(p[1])'; "
        "printf '%s\n' 'int main(void) { return 0; }' "
        "> /root/unroot-qualification.c; "
        "/usr/bin/gcc -O0 -o /root/unroot-qualification "
        "/root/unroot-qualification.c; "
        "/root/unroot-qualification; "
        "printf 'created through unroot\n' > /root/unroot-heritage; "
        "chmod 0640 /root/unroot-heritage; "
        "/usr/bin/gcc -dumpmachine; "
        "/usr/bin/stat -c %u:%g /var/lib/portage/world",
        timeout=300,
    ).assert_ok()
    chost, portage_ids = entered.stdout.splitlines()
    portage_uid, portage_gid = (int(value) for value in portage_ids.split(":"))
    assert chost == expected_chost
    assert portage_uid == 0
    assert portage_gid > 0
    assert marker.read_text(encoding="utf-8") == "created through unroot\n"
    assert stat.S_IMODE(marker.stat().st_mode) == 0o640
    assert binary.is_file()

    captured = tmp_path / "captured.tar"
    unroot.run("pack", str(root), str(captured), timeout=300).assert_ok()

    restored = tmp_path / "restored"
    unroot.run("unpack", str(captured), str(restored), timeout=300).assert_ok()
    restored_marker = restored / "root" / "unroot-heritage"
    assert restored_marker.read_text(encoding="utf-8") == (
        "created through unroot\n"
    )
    assert stat.S_IMODE(restored_marker.stat().st_mode) == 0o640

    verified = unroot.run(
        "enter",
        str(restored),
        "--",
        "/bin/bash",
        "-c",
        "set -eu; "
        "test \"$(cat /root/unroot-heritage)\" = 'created through unroot'; "
        "/root/unroot-qualification; "
        "/usr/bin/gcc -dumpmachine; "
        "/usr/bin/stat -c %u:%g /var/lib/portage/world",
        timeout=300,
    ).assert_ok()
    assert verified.stdout.splitlines() == [expected_chost, portage_ids]
