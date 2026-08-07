from __future__ import annotations

import os
import signal
import subprocess
import sys
from pathlib import Path

import pytest

from .support import UnrootRunner, create_rootfs


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.namespace_policy,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def test_restricted_user_namespace_reports_classified_failure(
    unroot: UnrootRunner,
) -> None:
    if os.environ.get("UNROOT_E2E_USERNS_POLICY") != "restricted":
        pytest.skip("requires an explicitly restricted user-namespace policy")

    result = unroot.run("single", "--", "/usr/bin/id", "-u")

    assert result.returncode not in {-signal.SIGPIPE, 128 + signal.SIGPIPE}, (
        result.diagnostic()
    )
    expected = {
        4: "failed to set MS_PRIVATE",
        10: "unshare failed",
        29: "setgroups deny failed",
    }
    assert result.returncode in expected, result.diagnostic()
    assert expected[result.returncode] in result.stderr, result.diagnostic()
    assert any(
        message in result.stderr
        for message in ("Operation not permitted", "Permission denied")
    ), result.diagnostic()
    detail = os.environ.get("UNROOT_E2E_POLICY_DETAIL", "apparmor")
    if detail == "apparmor":
        assert "AppArmor" in result.stderr, result.diagnostic()
        assert "kernel.apparmor_restrict_unprivileged_userns=1" in result.stderr, (
            result.diagnostic()
        )
    elif detail == "container":
        assert "seccomp, or outer-container policy" in result.stderr, (
            result.diagnostic()
        )
    else:
        pytest.fail(f"unknown policy detail expectation: {detail}")


def test_restricted_system_paths_report_idmap_failure(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    if os.environ.get("UNROOT_E2E_SYSTEMPATHS_POLICY") != "restricted":
        pytest.skip("requires explicitly restricted container system paths")

    source = create_rootfs(tmp_path / "source")
    archive = tmp_path / "rootfs.tar"
    subprocess.run(
        [
            "tar",
            "--create",
            "--format=pax",
            "--numeric-owner",
            "--owner=0",
            "--group=0",
            f"--file={archive}",
            "--directory",
            str(source),
            ".",
        ],
        check=True,
    )
    result = unroot.run("unpack", str(archive), str(tmp_path / "root"))

    assert result.returncode == 24, result.diagnostic()
    assert "newuidmap failed" in result.stderr, result.diagnostic()
    assert any(
        message in result.stderr
        for message in ("Operation not permitted", "Permission denied")
    ), (
        result.diagnostic()
    )
