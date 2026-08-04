from __future__ import annotations

import json
import os
import platform
import shlex
import shutil
import sys
from pathlib import Path
from typing import Callable, Optional, Tuple

import pytest

from .support import UnrootRunner, run_command


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.cross_arch,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


def _foreign_program(
    root: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> Tuple[Path, str, str]:
    configured = os.environ.get("FOREIGN_BINARY")
    if configured:
        source = Path(configured)
        require_capability(
            source.is_file() and os.access(source, os.X_OK),
            f"FOREIGN_BINARY is not executable: {source}",
            "cross_arch",
        )
        name = os.environ.get("FOREIGN_NAME", source.name)
        destination = root / "bin" / name
        shutil.copy2(source, destination)
        destination.chmod(0o755)
        return (
            destination,
            os.environ.get("FOREIGN_EXPECTED", "foreign-ok"),
            str(source),
        )

    compiler = os.environ.get("CROSS_CC", "")
    require_capability(
        bool(compiler) and shutil.which(compiler) is not None,
        "set CROSS_CC to a static-capable cross compiler",
        "cross_arch",
    )
    source = root / "probe.c"
    source.write_text(
        "#include <unistd.h>\n"
        "int main(void) {\n"
        '  static const char message[] = "foreign-ok\\n";\n'
        "  return write(1, message, sizeof(message) - 1) < 0;\n"
        "}\n",
        encoding="utf-8",
    )
    destination = root / "bin" / "probe"
    result = run_command(
        [compiler, "-static", "-O2", "-o", str(destination), str(source)]
    )
    result.assert_ok()
    source.unlink()
    return destination, "foreign-ok", compiler


def _default_foreign_qemu() -> Optional[str]:
    machine = platform.machine().lower()
    name = {
        "x86_64": "qemu-aarch64-static",
        "amd64": "qemu-aarch64-static",
        "aarch64": "qemu-x86_64-static",
        "arm64": "qemu-x86_64-static",
    }.get(machine)
    return shutil.which(name) if name else None


def test_foreign_architecture_execution(
    unroot: UnrootRunner,
    empty_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    program, expected, source = _foreign_program(empty_rootfs, require_capability)
    arguments = shlex.split(os.environ.get("FOREIGN_ARGS", ""))
    result = unroot.run(
        "enter",
        str(empty_rootfs),
        "--",
        "/bin/" + program.name,
        *arguments,
        timeout=90,
    )
    assert result.returncode == 0, (
        f"foreign fixture from {source} failed\n{result.diagnostic()}"
    )
    assert result.stdout.strip() == expected


def test_foreign_shebang_execution(
    unroot: UnrootRunner,
    empty_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    program, expected, source = _foreign_program(empty_rootfs, require_capability)
    script = empty_rootfs / "bin" / "foreign-script"
    script.write_text(f"#!/bin/{program.name}\n", encoding="utf-8")
    script.chmod(0o755)

    result = unroot.run(
        "enter",
        str(empty_rootfs),
        "--",
        "/bin/foreign-script",
        timeout=90,
    )
    assert result.returncode == 0, (
        f"foreign shebang fixture using {source} failed\n{result.diagnostic()}"
    )
    assert result.stdout.strip() == expected


def test_foreign_execution_can_be_disabled(
    unroot: UnrootRunner,
    empty_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    program, _, _ = _foreign_program(empty_rootfs, require_capability)
    result = unroot.run(
        "enter", str(empty_rootfs), "--emulation", "never", "--",
        "/bin/" + program.name,
    )
    assert result.returncode != 0, result.diagnostic()
    assert "requires emulation" in result.stderr


def test_explicit_qemu_and_cpu_model_reach_the_wrapper(
    unroot: UnrootRunner,
    empty_rootfs: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    program, expected, source = _foreign_program(empty_rootfs, require_capability)
    qemu = _default_foreign_qemu()
    cpu = os.environ.get("FOREIGN_QEMU_CPU")
    require_capability(
        qemu is not None and bool(cpu),
        "a static foreign QEMU and FOREIGN_QEMU_CPU are required",
        "cross_arch",
    )
    result = unroot.run(
        "enter", str(empty_rootfs), "--qemu", qemu, "--qemu-cpu", cpu,
        "--", "/bin/" + program.name,
        timeout=90,
    )
    assert result.returncode == 0, (
        f"foreign fixture from {source} failed\n{result.diagnostic()}"
    )
    assert result.stdout.strip() == expected
    metadata = json.loads(
        (empty_rootfs / ".unroot" / "meta.json").read_text(encoding="utf-8")
    )
    assert metadata["qemuWrapper"]["emuArgs"] == ["-cpu", cpu]


def test_native_qemu_rejects_a_user_controlled_parent_directory(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
    privileged_prefix: tuple[str, ...],
) -> None:
    root = tmp_path / "root"
    (root / "bin").mkdir(parents=True)
    program, _, _ = _foreign_program(root, require_capability)
    qemu = _default_foreign_qemu()
    require_capability(
        qemu is not None,
        "a static foreign QEMU is required",
        "cross_arch",
    )

    directory = tmp_path / "user-controlled"
    directory.mkdir()
    copied = directory / Path(qemu).name
    shutil.copy2(qemu, copied)
    run_command(
        [*privileged_prefix, "chown", "root:root", str(copied)]
    ).assert_ok()

    result = run_command(
        [
            *privileged_prefix,
            str(unroot.binary),
            "enter",
            "--native",
            str(root),
            "--qemu",
            str(copied),
            "--",
            "/bin/" + program.name,
        ]
    )

    assert result.returncode != 0, result.diagnostic()
    assert "trusted root-controlled path" in result.stderr
