from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Callable, Optional, Set

import pytest

from .support import (
    UnrootRunner,
    create_rootfs,
    find_static_busybox,
    run_command,
    snapshot_host,
)


REPO = Path(__file__).resolve().parents[2]


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("unroot-e2e")
    group.addoption(
        "--unroot-bin",
        default=os.environ.get("UNROOT_BIN", str(REPO / "bin" / "unroot")),
        help="path to the Unroot executable under test",
    )
    group.addoption(
        "--strict-e2e",
        action="store_true",
        default=os.environ.get("UNROOT_E2E_STRICT") == "1",
        help="fail instead of skip when a required host capability is unavailable",
    )
    group.addoption(
        "--e2e-report",
        default=os.environ.get("UNROOT_E2E_REPORT", ""),
        help="write a JSON host qualification report to this path",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line("markers", "e2e: executes the shipped Unroot binary")
    config.addinivalue_line(
        "markers",
        "cross_arch: requires a foreign static executable and QEMU user emulator",
    )
    config.addinivalue_line(
        "markers", "private_binfmt: exercises a private binfmt_misc mount"
    )
    config.addinivalue_line(
        "markers", "namespace_policy: exercises an explicitly configured host policy"
    )
    config.addinivalue_line(
        "markers",
        "rich_idmap: requires unroot-util, subordinate IDs, newuidmap, and newgidmap",
    )
    config.addinivalue_line(
        "markers",
        "rootfs_journey: exercises a complete real-distribution rootfs lifecycle",
    )


def pytest_report_header(config: pytest.Config) -> str:
    host = snapshot_host()
    return (
        f"unroot host: kernel={host.kernel} machine={host.machine} euid={host.euid} "
        f"seccomp={host.seccomp or 'unknown'} userns_max={host.max_user_namespaces or 'unknown'} "
        f"apparmor_userns={host.apparmor_restrict_unprivileged_userns or 'n/a'} "
        f"binfmt={'mounted' if host.binfmt.mounted else 'unmounted'}"
    )


def pytest_sessionfinish(session: pytest.Session, exitstatus: int) -> None:
    destination = session.config.getoption("--e2e-report")
    if not destination:
        return
    report = {
        "exit_status": exitstatus,
        "required_capabilities": sorted(
            name.strip()
            for name in os.environ.get("UNROOT_E2E_REQUIRE", "").split(",")
            if name.strip()
        ),
        "host": snapshot_host().to_dict(),
    }
    path = Path(destination)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


@pytest.fixture(scope="session")
def strict_e2e(pytestconfig: pytest.Config) -> bool:
    return bool(pytestconfig.getoption("--strict-e2e"))


@pytest.fixture(scope="session")
def required_capabilities() -> Set[str]:
    return {
        name.strip()
        for name in os.environ.get("UNROOT_E2E_REQUIRE", "").split(",")
        if name.strip()
    }


@pytest.fixture(scope="session")
def require_capability(
    strict_e2e: bool,
    required_capabilities: Set[str],
) -> Callable[[bool, str, Optional[str]], None]:
    def require(available: bool, reason: str, capability: Optional[str] = None) -> None:
        if available:
            return
        if strict_e2e and (capability is None or capability in required_capabilities):
            pytest.fail(reason)
        pytest.skip(reason)

    return require


@pytest.fixture(scope="session")
def privileged_prefix(
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> tuple[str, ...]:
    if os.geteuid() == 0:
        return ()
    sudo = shutil.which("sudo")
    if sudo is not None and run_command([sudo, "-n", "true"]).returncode == 0:
        return (sudo, "-n")
    require_capability(
        False,
        "passwordless sudo is required for native-mode qualification",
        "native_mode",
    )
    raise RuntimeError("unreachable")


@pytest.fixture(scope="session")
def unroot_binary(pytestconfig: pytest.Config) -> Path:
    binary = Path(pytestconfig.getoption("--unroot-bin")).resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        pytest.fail(f"Unroot executable not found or not executable: {binary}")
    return binary


@pytest.fixture(scope="session")
def sudo_guard(tmp_path_factory: pytest.TempPathFactory) -> Path:
    guard = tmp_path_factory.mktemp("unroot-safety") / "deny-sudo"
    guard.write_text(
        "#!/bin/sh\necho 'E2E safety: host privilege escalation attempted' >&2\nexit 125\n",
        encoding="utf-8",
    )
    guard.chmod(0o700)
    return guard


@pytest.fixture
def unroot(unroot_binary: Path, sudo_guard: Path) -> UnrootRunner:
    return UnrootRunner(unroot_binary, REPO, sudo_guard)


@pytest.fixture(autouse=True)
def preserve_host_binfmt() -> None:
    before = snapshot_host()
    yield
    after = snapshot_host()
    assert after.binfmt == before.binfmt, (
        "host binfmt_misc state changed during an E2E test\n"
        f"before:\n{before.binfmt.describe()}\n"
        f"after:\n{after.binfmt.describe()}"
    )


@pytest.fixture
def managed_rootfs(
    tmp_path: Path,
    unroot: UnrootRunner,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> Path:
    busybox = find_static_busybox()
    require_capability(
        busybox is not None,
        "a static BusyBox is required; install busybox-static or set BUSYBOX",
    )
    return _managed_rootfs(tmp_path, unroot, require_capability, busybox)


@pytest.fixture
def empty_rootfs(
    tmp_path: Path,
    unroot: UnrootRunner,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> Path:
    return _managed_rootfs(tmp_path, unroot, require_capability)


def _managed_rootfs(
    tmp_path: Path,
    unroot: UnrootRunner,
    require_capability: Callable[[bool, str, Optional[str]], None],
    busybox: Optional[Path] = None,
) -> Path:
    source = create_rootfs(tmp_path / "source", busybox)
    archive = tmp_path / "rootfs.tar"
    subprocess.run(
        [
            "tar", "--create", "--format=pax", "--numeric-owner",
            "--owner=0", "--group=0", f"--file={archive}",
            "--directory", str(source), ".",
        ],
        check=True,
    )
    root = tmp_path / "root"
    result = unroot.run("unpack", str(archive), str(root))
    require_capability(
        result.returncode == 0,
        "managed rootfs setup requires rich ID mapping:\n" + result.diagnostic(),
        "rich_idmap",
    )
    return root
