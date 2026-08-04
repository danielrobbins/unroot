from __future__ import annotations

import json
import os
import platform
import signal
import shutil
import struct
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, Mapping, Optional, Sequence, Tuple


@dataclass(frozen=True)
class BinfmtSnapshot:
    mounted: bool
    device: Optional[int]
    files: Tuple[Tuple[str, str], ...]

    def to_dict(self) -> Dict[str, object]:
        return asdict(self)

    def describe(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True)


@dataclass(frozen=True)
class HostSnapshot:
    kernel: str
    machine: str
    euid: int
    egid: int
    seccomp: Optional[str]
    no_new_privs: Optional[str]
    unprivileged_userns_clone: Optional[str]
    apparmor_restrict_unprivileged_userns: Optional[str]
    max_user_namespaces: Optional[str]
    binfmt: BinfmtSnapshot

    def to_dict(self) -> Dict[str, object]:
        return asdict(self)

    def describe(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True)


@dataclass(frozen=True)
class CommandResult:
    command: Tuple[str, ...]
    returncode: int
    stdout: str
    stderr: str

    def diagnostic(self) -> str:
        rendered = " ".join(self.command)
        return (
            f"command: {rendered}\n"
            f"exit: {self.returncode}\n"
            f"stdout:\n{self.stdout}\n"
            f"stderr:\n{self.stderr}"
        )

    def assert_ok(self) -> "CommandResult":
        assert self.returncode == 0, self.diagnostic()
        return self


def _read_text(path: Path) -> Optional[str]:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def snapshot_binfmt() -> BinfmtSnapshot:
    root = Path("/proc/sys/fs/binfmt_misc")
    status = root / "status"
    if not status.is_file():
        return BinfmtSnapshot(False, None, ())

    files = []
    try:
        children: Iterable[Path] = sorted(root.iterdir(), key=lambda path: path.name)
    except OSError:
        children = ()
    for child in children:
        if child.name == "register":
            continue
        text = _read_text(child)
        files.append((child.name, text if text is not None else "<unreadable>"))

    try:
        device = root.stat().st_dev
    except OSError:
        device = None
    return BinfmtSnapshot(True, device, tuple(files))


def _status_value(name: str) -> Optional[str]:
    status = _read_text(Path("/proc/self/status"))
    if status is None:
        return None
    prefix = name + ":"
    for line in status.splitlines():
        if line.startswith(prefix):
            return line.split(":", 1)[1].strip()
    return None


def snapshot_host() -> HostSnapshot:
    geteuid = getattr(os, "geteuid", lambda: -1)
    getegid = getattr(os, "getegid", lambda: -1)
    return HostSnapshot(
        kernel=platform.release(),
        machine=platform.machine(),
        euid=geteuid(),
        egid=getegid(),
        seccomp=_status_value("Seccomp"),
        no_new_privs=_status_value("NoNewPrivs"),
        unprivileged_userns_clone=_read_text(
            Path("/proc/sys/kernel/unprivileged_userns_clone")
        ),
        apparmor_restrict_unprivileged_userns=_read_text(
            Path("/proc/sys/kernel/apparmor_restrict_unprivileged_userns")
        ),
        max_user_namespaces=_read_text(Path("/proc/sys/user/max_user_namespaces")),
        binfmt=snapshot_binfmt(),
    )


class UnrootRunner:
    def __init__(self, binary: Path, repo: Path, sudo_guard: Path) -> None:
        self.binary = binary
        self.repo = repo
        self.sudo_guard = sudo_guard

    def run(
        self,
        *arguments: str,
        env: Optional[Mapping[str, str]] = None,
        timeout: int = 30,
    ) -> CommandResult:
        command = (str(self.binary),) + tuple(str(argument) for argument in arguments)
        process_env: Dict[str, str] = dict(os.environ)
        for name in tuple(process_env):
            if name.startswith("UNROOT_"):
                process_env.pop(name)
        process_env["UNROOT_SUDO"] = str(self.sudo_guard)
        process_env["LC_ALL"] = "C"
        if env:
            process_env.update(env)

        process = subprocess.Popen(
            command,
            cwd=self.repo,
            env=process_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"command timed out after {timeout}s: {' '.join(command)}\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            ) from error

        return CommandResult(command, process.returncode, stdout, stderr)


def find_static_busybox() -> Optional[Path]:
    configured = os.environ.get("BUSYBOX")
    candidate = Path(configured) if configured else Path(shutil.which("busybox") or "")
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        return None
    return candidate if is_static_elf(candidate) else None


def is_static_elf(path: Path) -> bool:
    try:
        with path.open("rb") as source:
            header = source.read(64)
            if header[:4] != b"\x7fELF" or header[4] not in (1, 2):
                return False
            byte_order = "<" if header[5] == 1 else ">" if header[5] == 2 else ""
            if not byte_order:
                return False
            if header[4] == 2:
                program_offset = struct.unpack_from(byte_order + "Q", header, 32)[0]
                entry_size, entry_count = struct.unpack_from(
                    byte_order + "HH", header, 54
                )
            else:
                program_offset = struct.unpack_from(byte_order + "I", header, 28)[0]
                entry_size, entry_count = struct.unpack_from(
                    byte_order + "HH", header, 42
                )
            if not program_offset or not entry_size or not entry_count:
                return False
            for index in range(entry_count):
                source.seek(program_offset + index * entry_size)
                entry = source.read(4)
                if len(entry) != 4:
                    return False
                if struct.unpack(byte_order + "I", entry)[0] == 3:  # PT_INTERP
                    return False
            return True
    except (OSError, struct.error):
        return False


def create_rootfs(root: Path, busybox: Optional[Path] = None) -> Path:
    for directory in ("bin", "dev", "etc", "proc", "tmp"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    if busybox is not None:
        destination = root / "bin" / "busybox"
        shutil.copy2(busybox, destination)
        destination.chmod(0o755)
        (root / "bin" / "sh").symlink_to("busybox")
    return root


def run_command(command: Sequence[str], *, timeout: int = 30) -> CommandResult:
    completed = subprocess.run(
        tuple(command),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    return CommandResult(
        tuple(command), completed.returncode, completed.stdout, completed.stderr
    )


def probe_private_binfmt(target: Path, entry: str) -> CommandResult:
    unshare = shutil.which("unshare")
    mount = shutil.which("mount")
    if unshare is None or mount is None:
        missing = "unshare" if unshare is None else "mount"
        return CommandResult(
            (missing,), 127, "", f"required command not found: {missing}"
        )
    target.mkdir()
    script = r"""
set -eu
mount -t binfmt_misc binfmt_misc "$1"
child_device=$(stat -c %d "$1")
test "$child_device" != "$3"
printf ':%s:E::unroot_e2e_probe::/bin/true:' "$2" > "$1/register"
test -e "$1/$2"
printf -- '-1' > "$1/$2"
test ! -e "$1/$2"
printf 'private-binfmt-ok:%s' "$child_device"
"""
    host_device = snapshot_binfmt().device
    return run_command(
        [
            unshare,
            "--user",
            "--map-root-user",
            "--mount",
            "sh",
            "-c",
            script,
            "sh",
            str(target),
            entry,
            str(host_device),
        ]
    )
