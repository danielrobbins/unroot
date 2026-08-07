from __future__ import annotations

import json
import os
import shutil
import sys
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import (
    UnrootRunner,
    create_rootfs,
    find_static_busybox,
    probe_private_binfmt,
    run_command,
)


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def test_public_help_surface(unroot: UnrootRunner) -> None:
    global_help = unroot.run("--help").assert_ok()
    assert "[-- COMMAND [ARGUMENTS...]]" in global_help.stdout
    assert "--debug" in global_help.stdout
    assert "pack ROOT ARCHIVE" in global_help.stdout
    assert "unpack ARCHIVE ROOT" in global_help.stdout
    assert "prepare" not in global_help.stdout

    enter_help = unroot.run("enter", "--help").assert_ok()
    assert "enter enter" not in enter_help.stdout
    assert "[-- COMMAND [ARGUMENTS...]]" in enter_help.stdout
    assert "--native" in enter_help.stdout
    assert "--cwd" in enter_help.stdout
    assert "--env" in enter_help.stdout
    assert "--no-default-env" in enter_help.stdout
    assert "--emulation" in enter_help.stdout
    assert "--qemu" in enter_help.stdout
    assert "--qemu-cpu" in enter_help.stdout
    assert "--single" in enter_help.stdout
    assert "--subarch" not in enter_help.stdout
    assert "experimental" not in enter_help.stdout

    pack_help = unroot.run("pack", "--help").assert_ok()
    assert "pack ROOT ARCHIVE" in pack_help.stdout
    unpack_help = unroot.run("unpack", "--help").assert_ok()
    assert "unpack ARCHIVE ROOT" in unpack_help.stdout
    assert "--native" in unpack_help.stdout

@pytest.mark.parametrize("arguments", [("unknown-action",), ("unknown-action", "--help")])
def test_unknown_action_is_usage_error(
    unroot: UnrootRunner, arguments: tuple[str, ...]
) -> None:
    result = unroot.run(*arguments)

    assert result.returncode == 2, result.diagnostic()
    assert "unknown action: unknown-action" in result.stderr


@pytest.mark.parametrize("action", ["fs", "fs.mkdir", "unroot-internal-fs"])
def test_deferred_filesystem_actions_are_not_public(
    unroot: UnrootRunner, action: str
) -> None:
    result = unroot.run(action)

    assert result.returncode != 0, result.diagnostic()
    assert f"unknown action: {action}" in result.stderr


def test_standalone_single_action_is_not_public(unroot: UnrootRunner) -> None:
    result = unroot.run("single", "--", "/bin/true")

    assert result.returncode == 2, result.diagnostic()
    assert "unknown action: single" in result.stderr


def test_enter_single_changes_root_with_one_id_mapping(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    busybox = find_static_busybox()
    require_capability(
        busybox is not None,
        "a static BusyBox is required for rooted single-ID entry coverage",
        "single_rootfs",
    )
    root = create_rootfs(tmp_path / "single-root", busybox)
    (root / "inside-root").write_text("rooted-single-ok", encoding="utf-8")

    result = unroot.run(
        "enter",
        "--single",
        str(root),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'printf "%s:" "$(id -u)"; cat /inside-root; '
        'printf ":%s" "$(cat /proc/self/setgroups)"',
    ).assert_ok()

    assert result.stdout == "0:rooted-single-ok:deny"


def test_enter_single_rejects_managed_ownership(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter", "--single", str(managed_rootfs), "--", "/bin/busybox", "true"
    )

    assert result.returncode != 0, result.diagnostic()
    assert (
        "--single conflicts with the ownership mode recorded for ROOT"
        in result.stderr
    )


def test_enter_single_does_not_require_host_helper(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    busybox = find_static_busybox()
    if busybox is None:
        pytest.skip("a static BusyBox is required for rooted single-ID entry")
    root = create_rootfs(tmp_path / "single-root", busybox)
    standalone = tmp_path / "standalone" / "unroot"
    standalone.parent.mkdir()
    shutil.copy2(unroot.binary, standalone)
    isolated = UnrootRunner(standalone, unroot.repo, unroot.sudo_guard)

    result = isolated.run(
        "enter", "--single", str(root), "--", "/bin/busybox", "id", "-u"
    ).assert_ok()

    assert result.stdout.strip() == "0"




def test_managed_rooted_execution(unroot: UnrootRunner, managed_rootfs: Path) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--cwd",
        "/tmp",
        "--env",
        "E2E_VALUE=ok",
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'printf "%s:%s:%s" "$(id -u)" "$PWD" "$E2E_VALUE"',
    ).assert_ok()
    assert result.stdout == "0:/tmp:ok"


def test_managed_rootfs_can_allocate_pty(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        "test -L /dev/ptmx && exec 3<>/dev/ptmx",
    )

    result.assert_ok()


def test_native_rootfs_has_usable_minimal_devices(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    rootfs = create_rootfs(tmp_path / "minimal-dev", find_static_busybox())
    result = run_command(
        [
            *privileged_prefix,
            str(unroot.binary),
            "enter",
            "--native",
            str(rootfs),
            "--",
            "/bin/busybox",
            "sh",
            "-c",
            """
set -eu
test -c /dev/null
test -c /dev/full
test -L /dev/fd
test -L /dev/stdin
test -L /dev/stdout
test -L /dev/stderr
test -L /dev/ptmx
exec 3</dev/null
exec 4<>/dev/ptmx
cat /dev/fd/3
printf minimal-dev-ok >/dev/stdout
""",
        ]
    ).assert_ok()

    assert result.stdout == "minimal-dev-ok"


def test_native_rootfs_shares_host_resolver_files(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    rootfs = create_rootfs(tmp_path / "resolver-files", find_static_busybox())
    for name in ("resolv.conf", "hosts"):
        result = run_command(
            [
                *privileged_prefix,
                str(unroot.binary),
                "enter",
                "--native",
                str(rootfs),
                "--",
                "/bin/busybox",
                "cat",
                f"/etc/{name}",
            ]
        ).assert_ok()

        assert result.stdout == Path(f"/etc/{name}").read_text(encoding="utf-8")


def test_qemu_overrides_reject_native_targets(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter", str(managed_rootfs), "--qemu", "/bin/true", "--",
        "/bin/busybox", "true",
    )
    assert result.returncode != 0, result.diagnostic()
    assert "require a foreign-architecture target" in result.stderr


def test_managed_rootfs_records_rich_ownership(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    metadata = json.loads(
        (managed_rootfs / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["idmap"]["mode"] == "rich"
    assert len(metadata["idmap"]["uid_map"]) == 2


def test_unmanaged_rootfs_requires_explicit_native_mode(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    root = create_rootfs(tmp_path / "plain", find_static_busybox())
    result = unroot.run("enter", str(root), "--", "/bin/busybox", "true")
    assert result.returncode != 0
    assert "has no Unroot ownership metadata" in result.stderr
    assert not (root / ".unroot").exists()

    native = unroot.run(
        "enter", "--native", str(root), "--", "/bin/busybox", "true"
    )
    assert native.returncode != 0
    assert "requires host root privileges" in native.stderr


def test_explicit_native_entry_does_not_adopt_an_unmanaged_rootfs(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
) -> None:
    busybox = find_static_busybox()
    if busybox is None:
        pytest.skip("a static BusyBox is required for native entry coverage")
    root = create_rootfs(tmp_path / "plain-native", busybox)
    command = [
        str(unroot.binary),
        "enter",
        "--native",
        str(root),
        "--",
        "/bin/busybox",
        "id",
        "-u",
    ]
    command = [*privileged_prefix, *command]

    result = run_command(command).assert_ok()

    assert result.stdout.strip() == "0"
    assert not (root / ".unroot").exists()


def test_host_environment_is_not_inherited(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'test -z "${E2E_HOST_ONLY+x}"',
        env={"E2E_HOST_ONLY": "must-not-leak"},
    )
    result.assert_ok()


def test_persist_env_selectively_inherits_host_value(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--persist-env",
        "E2E_PERSISTED",
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'printf "%s" "$E2E_PERSISTED"',
        env={"E2E_PERSISTED": "selected"},
    ).assert_ok()
    assert result.stdout == "selected"


def test_rooted_path_resolves_executable(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--env",
        "PATH=/bin",
        "--",
        "busybox",
        "printf",
        "path-ok",
    ).assert_ok()
    assert result.stdout == "path-ok"


@pytest.mark.parametrize("path", [".", ":/bin"])
def test_path_resolves_from_working_directory(
    unroot: UnrootRunner, managed_rootfs: Path, path: str
) -> None:
    work = managed_rootfs / "work"
    work.mkdir()
    tool = work / "e2e-tool"
    tool.write_text("#!/bin/sh\nprintf path-cwd-ok\n", encoding="utf-8")
    tool.chmod(0o755)

    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--cwd",
        "/work",
        "--env",
        f"PATH={path}",
        "--",
        "e2e-tool",
    ).assert_ok()
    assert result.stdout == "path-cwd-ok"


def test_relative_executable_resolves_from_working_directory(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    work = managed_rootfs / "src"
    work.mkdir()
    configure = work / "configure"
    configure.write_text("#!/bin/sh\nprintf relative-ok\n", encoding="utf-8")
    configure.chmod(0o755)

    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--cwd",
        "/src",
        "--",
        "./configure",
    ).assert_ok()
    assert result.stdout == "relative-ok"


def test_bare_command_uses_default_path(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter", str(managed_rootfs), "--", "busybox", "printf", "default-path-ok"
    ).assert_ok()
    assert result.stdout == "default-path-ok"


def test_no_default_env_requires_path_for_bare_command(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter", str(managed_rootfs), "--no-default-env", "--", "busybox", "true"
    )

    assert result.returncode != 0, result.diagnostic()
    assert "bare commands require PATH via --env or --persist-env" in result.stderr


def test_absolute_command_receives_default_path(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'printf "%s" "$PATH"',
    ).assert_ok()
    assert result.stdout == "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"


def test_explicit_path_wins_over_persisted_and_default_path(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--persist-env",
        "PATH",
        "--env",
        "PATH=/bin",
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'printf "%s" "$PATH"',
        env={"PATH": "/host-only"},
    ).assert_ok()
    assert result.stdout == "/bin"


def test_missing_command_in_explicit_path(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--env",
        "PATH=/bin",
        "--",
        "missing-e2e-command",
    )

    assert result.returncode == 12, result.diagnostic()
    assert "enter failed: exec failed" in result.stderr, result.diagnostic()


def test_rooted_shebang_script(unroot: UnrootRunner, managed_rootfs: Path) -> None:
    script = managed_rootfs / "bin" / "e2e-script"
    script.write_text(
        "#!/bin/sh\nprintf 'shebang-ok:%s' \"$E2E_VALUE\"\n",
        encoding="utf-8",
    )
    script.chmod(0o755)

    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--env",
        "E2E_VALUE=inside",
        "--",
        "/bin/e2e-script",
    ).assert_ok()
    assert result.stdout == "shebang-ok:inside"


def test_rooted_absolute_executable_symlink(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    (managed_rootfs / "bin" / "printf").symlink_to("/bin/busybox")

    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/printf",
        "absolute-link-ok",
    ).assert_ok()

    assert result.stdout == "absolute-link-ok"


def test_readonly_file_map_rejects_writes(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    busybox = find_static_busybox()
    require_capability(
        busybox is not None,
        "a static BusyBox is required for read-only mapping coverage",
    )
    root = create_rootfs(tmp_path / "single-root", busybox)
    source = tmp_path / "mapped-source"
    source.write_text("host-content\n", encoding="utf-8")

    unroot.run(
        "enter",
        "--single",
        str(root),
        "--map-ro",
        str(source),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        'test "$(cat "$1")" = host-content && ! printf changed > "$1"',
        "sh",
        str(source),
    ).assert_ok()

    assert source.read_text(encoding="utf-8") == "host-content\n"


def test_readonly_directory_map_protects_nested_mounts(
    unroot: UnrootRunner,
    tmp_path: Path,
    privileged_prefix: tuple[str, ...],
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    unshare = shutil.which("unshare")
    mount = shutil.which("mount")
    if unshare is None or mount is None:
        pytest.skip("unshare and mount are required for nested-mount coverage")

    busybox = find_static_busybox()
    require_capability(
        busybox is not None,
        "a static BusyBox is required for nested-mount coverage",
    )
    root = create_rootfs(tmp_path / "native-root", busybox)
    source = tmp_path / "mapped-directory"
    nested = source / "nested"
    nested.mkdir(parents=True)
    script = r"""
set -eu
mount --make-rprivate /
mount -t tmpfs tmpfs "$1/nested"
printf original > "$1/nested/value"
"$3" enter --native "$2" --map-ro "$1:/tmp/mapped" -- \
  /bin/busybox sh -c '! printf changed > /tmp/mapped/nested/value'
test "$(cat "$1/nested/value")" = original
"""
    result = run_command(
        [
            *privileged_prefix,
            unshare,
            "--mount",
            "sh",
            "-c",
            script,
            "sh",
            str(source),
            str(root),
            str(unroot.binary),
        ]
    )

    assert result.returncode == 0, result.diagnostic()


def test_bind_staging_rejects_symlinked_directory(
    unroot: UnrootRunner, managed_rootfs: Path, tmp_path: Path
) -> None:
    outside = tmp_path / "outside-dev"
    outside.mkdir()
    (managed_rootfs / "dev").rmdir()
    (managed_rootfs / "dev").symlink_to(outside, target_is_directory=True)

    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "true",
    )

    assert result.returncode != 0, result.diagnostic()
    assert list(outside.iterdir()) == []


def test_child_exit_status_propagates(
    unroot: UnrootRunner, managed_rootfs: Path
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        "exit 37",
    )
    assert result.returncode == 37, result.diagnostic()


@pytest.mark.parametrize(
    "status",
    [100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 201],
)
def test_child_exit_status_does_not_collide_with_setup_failures(
    unroot: UnrootRunner, managed_rootfs: Path, status: int
) -> None:
    result = unroot.run(
        "enter",
        str(managed_rootfs),
        "--",
        "/bin/busybox",
        "sh",
        "-c",
        f"exit {status}",
    )
    assert result.returncode == status, result.diagnostic()


@pytest.mark.private_binfmt
def test_binfmt_mount_is_private_and_writable(
    require_capability: Callable[[bool, str, Optional[str]], None],
    tmp_path: Path,
) -> None:
    name = f"unroot-e2e-{os.getpid()}"
    probe = probe_private_binfmt(tmp_path / "binfmt-probe", name)
    require_capability(
        probe.returncode == 0,
        "private binfmt_misc mounts are unavailable\n" + probe.diagnostic(),
        "private_binfmt",
    )
    assert probe.stdout.startswith("private-binfmt-ok:")
