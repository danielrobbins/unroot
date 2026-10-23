"""Ownership checks must distinguish missing metadata from unreadable metadata."""

import os
import sys
from pathlib import Path

import pytest

from .support import UnrootRunner


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(not sys.platform.startswith("linux"), reason="requires Linux"),
]


@pytest.mark.parametrize("action", ["enter", "pack"])
@pytest.mark.parametrize("single", [False, True])
@pytest.mark.parametrize("problem", ["unreadable", "directory", "symlink"])
def test_unreadable_ownership_metadata_is_not_unmanaged(
    unroot: UnrootRunner, tmp_path: Path, action: str, single: bool, problem: str
) -> None:
    if problem == "unreadable" and os.geteuid() == 0:
        pytest.skip("permission denial requires an unprivileged caller")
    root = tmp_path / "root"
    metadata = root / ".unroot" / "meta.json"
    metadata.parent.mkdir(parents=True)
    if problem == "directory":
        metadata.mkdir()
    elif problem == "symlink":
        metadata.symlink_to("missing")
    else:
        metadata.write_text("{}", encoding="utf-8")
        metadata.chmod(0)
    output = tmp_path / "output.tar"
    arguments = [action, *(["--single"] if single else []), str(root)]
    arguments += [str(output)] if action == "pack" else ["--", "/bin/true"]
    try:
        result = unroot.run(*arguments)
        assert result.returncode != 0, result.diagnostic()
        assert "unable to read rootfs ownership metadata" in result.stderr, result.diagnostic()
        assert not output.exists()
    finally:
        if problem == "unreadable":
            metadata.chmod(0o600)
