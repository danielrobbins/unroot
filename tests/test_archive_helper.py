import json
import os
import subprocess
from pathlib import Path

import pytest


pytestmark = pytest.mark.skipif(
    os.name != "posix", reason="unroot-util descriptor tests require POSIX"
)


def _run_helper(
    helper: Path, arguments: list[str], descriptor: int, cwd: Path
) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    return subprocess.run(
        [
            str(helper), "archive", arguments[0], "--fd", str(descriptor),
            *arguments[1:],
        ],
        cwd=cwd,
        env=environment,
        pass_fds=(descriptor,),
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


@pytest.mark.parametrize(
    ("filter_name", "archive_name", "expected_filters"),
    [
        ("none", "output.tar", []),
        ("xz", "output.tar.xz", ["xz"]),
    ],
)
def test_archive_helper_packs_sparse_tree(
    tmp_path: Path,
    filter_name: str,
    archive_name: str,
    expected_filters: list[str],
) -> None:
    root = Path(__file__).parents[1]
    helper = root / "bin" / "unroot-util"
    assert helper.is_file(), "build unroot-util before running helper tests"

    source = tmp_path / "source"
    source.mkdir()
    (source / "payload").write_text("payload\n", encoding="utf-8")
    (source / "NetLock_Arany_Fotanúsítvány").write_text(
        "utf-8 pathname\n", encoding="utf-8"
    )
    with (source / "sparse").open("wb") as sparse:
        sparse.seek(1024 * 1024)
        sparse.write(b"x")

    archive = tmp_path / archive_name
    with archive.open("wb") as output:
        packed = _run_helper(
            helper,
            ["pack", "--filter", filter_name],
            output.fileno(),
            source,
        )
    assert packed.returncode == 0, packed.stderr

    with archive.open("rb") as input_file:
        inspected = _run_helper(
            helper, ["inspect"], input_file.fileno(), root
        )
    assert inspected.returncode == 0, inspected.stderr
    protocol, report = inspected.stdout.split(" ", 1)
    assert protocol == "unroot-archive-v1"
    contents = json.loads(report)
    assert contents["filters"] == expected_filters
    assert contents["members"] == 4
    assert contents["metadata"]["sparse_files"]["count"] == 1
