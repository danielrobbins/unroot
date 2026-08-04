import hashlib
import re
from pathlib import Path

import pytest

from scripts import release


def test_release_metadata_supports_unroot_beta_versions():
    metadata = release.release_metadata("1.0_beta1\n", "1.0_beta1")

    assert metadata.version == "1.0_beta1"
    assert metadata.title == "1.0 beta1"
    assert metadata.prerelease is True


@pytest.mark.parametrize("version", ["1", "1.0-beta1", "v1.0", "1.0_beta 1"])
def test_release_metadata_rejects_unsupported_versions(version):
    with pytest.raises(ValueError, match="unsupported VERSION"):
        release.release_metadata(version)


def test_release_metadata_rejects_mismatched_tag():
    with pytest.raises(ValueError, match="does not match"):
        release.release_metadata("1.0_beta1", "1.0_beta2")


def test_extract_notes_selects_only_requested_release():
    text = """# Unroot 1.0_beta1

Current release.

# Unroot 0.9

Earlier release.
"""

    assert release.extract_notes(text, "1.0_beta1") == "Current release.\n"


def test_current_release_notes_match_version():
    root = Path(__file__).parents[1]
    version = (root / "VERSION").read_text(encoding="ascii").strip()
    notes = (root / "RELEASE_NOTES.md").read_text(encoding="utf-8")

    assert release.extract_notes(notes, version)


def test_checksums_require_and_hash_complete_architecture_set(tmp_path):
    metadata = release.release_metadata("1.0_beta1")
    contents = {"x86_64": b"x86 artifact", "arm64": b"arm artifact"}
    for architecture, data in contents.items():
        (tmp_path / f"unroot-1.0_beta1-linux-{architecture}").write_bytes(data)

    output = release.write_checksums(
        tmp_path, metadata, {"x86_64", "arm64"}
    )

    assert output.read_text(encoding="ascii").splitlines() == [
        f"{hashlib.sha256(contents['arm64']).hexdigest()}  "
        "unroot-1.0_beta1-linux-arm64",
        f"{hashlib.sha256(contents['x86_64']).hexdigest()}  "
        "unroot-1.0_beta1-linux-x86_64",
    ]


def test_checksums_cover_packages_and_source_but_not_release_notes(tmp_path):
    metadata = release.release_metadata("1.0_beta1")
    contents = {
        "unroot-1.0_beta1-linux-arm64": b"arm artifact",
        "unroot-1.0_beta1-linux-x86_64": b"x86 artifact",
        "unroot-1.0_beta1.tar.xz": b"source",
        "unroot_1.0~beta1-1~debian13.1_amd64.deb": b"deb",
        "unroot-1.0-0.1.beta1.fc44.x86_64.rpm": b"rpm",
    }
    for name, data in contents.items():
        (tmp_path / name).write_bytes(data)
    (tmp_path / "release-notes.md").write_text("notes", encoding="utf-8")

    output = release.write_checksums(
        tmp_path, metadata, {"x86_64", "arm64"}
    )

    assert output.read_text(encoding="ascii").splitlines() == [
        f"{hashlib.sha256(contents[name]).hexdigest()}  {name}"
        for name in sorted(contents)
    ]


def test_checksums_reject_incomplete_architecture_set(tmp_path):
    metadata = release.release_metadata("1.0_beta1")
    (tmp_path / "unroot-1.0_beta1-linux-x86_64").write_bytes(b"binary")

    with pytest.raises(ValueError, match="do not match"):
        release.write_checksums(tmp_path, metadata, {"x86_64", "arm64"})


def test_workflow_actions_are_pinned_and_publish_is_narrowly_privileged():
    workflows = Path(__file__).parents[1] / ".github" / "workflows"
    content = "\n".join(path.read_text() for path in workflows.glob("*.yml"))
    actions = re.findall(r"^\s*uses:\s+([^#\s]+)", content, re.MULTILINE)
    assert actions
    assert all(re.fullmatch(r"[^@]+@[0-9a-f]{40}", action) for action in actions)

    release_workflow = (workflows / "release.yml").read_text(encoding="utf-8")
    build, publish = release_workflow.split("\n  publish:", 1)
    assert "permissions:\n  contents: read" in build
    assert "contents: write" not in build
    assert "permissions:\n      contents: write" in publish
    assert "actions/checkout@" not in publish


def test_public_docs_match_the_initial_release_surface():
    root = Path(__file__).parents[1]
    manual = (root / "docs" / "unroot.docatoms").read_text(encoding="utf-8")
    readme = (root / "README.md").read_text(encoding="utf-8")
    notes = (root / "RELEASE_NOTES.md").read_text(encoding="utf-8")

    assert "unroot enter ROOT" in manual
    assert "unroot single" in manual
    assert "unroot unpack --native" in manual
    assert "not fallbacks for one another" in manual
    for obsolete in ("--rootless", "--idmap"):
        assert obsolete not in manual
        assert obsolete not in readme
        assert obsolete not in notes
    removed_actions = (
        "prepare", "register", "unregister", "list",
        "show", "scan", "init", "status",
    )
    for action in removed_actions:
        assert f"== @action {action}" not in manual
    assert "# Unroot 2." not in notes
