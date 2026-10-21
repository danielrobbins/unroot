from __future__ import annotations

import gzip
import hashlib
import io
import json
import os
import re
import sys
import tarfile
from pathlib import Path
from typing import Callable, Optional

import pytest

from .support import UnrootRunner


pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(
        os.name != "posix" or not sys.platform.startswith("linux"),
        reason="Unroot E2E requires Linux",
    ),
]


OCI_MANIFEST = "application/vnd.oci.image.manifest.v1+json"
OCI_CONFIG = "application/vnd.oci.image.config.v1+json"
OCI_LAYER = "application/vnd.oci.image.layer.v1.tar"
OCI_LAYER_GZIP = "application/vnd.oci.image.layer.v1.tar+gzip"


def _ownership_arguments() -> tuple[str, ...]:
    return ("--native",) if os.geteuid() == 0 else ()


def _json(value: object) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _digest(content: bytes) -> str:
    return "sha256:" + hashlib.sha256(content).hexdigest()


def _descriptor(media_type: str, content: bytes) -> dict[str, object]:
    return {
        "mediaType": media_type,
        "digest": _digest(content),
        "size": len(content),
    }


def _layer(entries: list[tuple[str, bytes | None, str]]) -> bytes:
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for name, content, kind in entries:
            member = tarfile.TarInfo(name)
            member.uid = 0
            member.gid = 0
            if kind == "dir":
                member.type = tarfile.DIRTYPE
                member.mode = 0o755
                archive.addfile(member)
            elif kind == "symlink":
                member.type = tarfile.SYMTYPE
                member.linkname = content.decode() if content else "target"
                archive.addfile(member)
            else:
                payload = content or b""
                member.mode = 0o644
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
    return output.getvalue()


def _oci_archive(
    destination: Path,
    layers: list[tuple[bytes, str]],
    *,
    architecture: str = "amd64",
    diff_ids: list[str] | None = None,
    corrupt_blob: str | None = None,
    index_variant: str | None = None,
    config_variant: str | None = None,
    manifest_media_type: str = OCI_MANIFEST,
    config_media_type: str = OCI_CONFIG,
    additional_platforms: tuple[tuple[str, str | None], ...] = (),
) -> dict[str, object]:
    blobs: dict[str, bytes] = {}
    layer_descriptors = []
    actual_diff_ids = []
    for uncompressed, media_type in layers:
        blob = (
            gzip.compress(uncompressed, mtime=0)
            if media_type == OCI_LAYER_GZIP
            else uncompressed
        )
        descriptor = _descriptor(media_type, blob)
        layer_descriptors.append(descriptor)
        actual_diff_ids.append(_digest(uncompressed))
        blobs[str(descriptor["digest"])] = blob

    def add_image(
        image_architecture: str,
        image_index_variant: str | None,
        image_config_variant: str | None,
    ) -> tuple[dict[str, object], dict[str, object]]:
        config_value = {
            "architecture": image_architecture,
            "os": "linux",
            "rootfs": {
                "type": "layers",
                "diff_ids": diff_ids or actual_diff_ids,
            },
            "config": {
                "Entrypoint": ["/bin/sh"],
                "Env": ["OCI_FIXTURE=yes"],
                "WorkingDir": "/work",
            },
        }
        if image_config_variant:
            config_value["variant"] = image_config_variant
        config = _json(config_value)
        config_descriptor = _descriptor(config_media_type, config)
        blobs[str(config_descriptor["digest"])] = config
        manifest = _json(
            {
                "schemaVersion": 2,
                "mediaType": manifest_media_type,
                "config": config_descriptor,
                "layers": layer_descriptors,
            }
        )
        manifest_descriptor = _descriptor(manifest_media_type, manifest)
        blobs[str(manifest_descriptor["digest"])] = manifest
        platform_value = {
            "architecture": image_architecture,
            "os": "linux",
        }
        if image_index_variant:
            platform_value["variant"] = image_index_variant
        return config_descriptor, {**manifest_descriptor, "platform": platform_value}

    config_descriptor, primary_manifest = add_image(
        architecture, index_variant, config_variant
    )
    manifests = [primary_manifest]
    for extra_architecture, extra_variant in additional_platforms:
        _, extra_manifest = add_image(
            extra_architecture, extra_variant, extra_variant
        )
        manifests.append(extra_manifest)
    index = _json(
        {
            "schemaVersion": 2,
            "manifests": manifests,
        }
    )

    if corrupt_blob:
        blobs[corrupt_blob] += b"corrupt"

    with tarfile.open(destination, "w", format=tarfile.PAX_FORMAT) as archive:
        root = tarfile.TarInfo(".")
        root.type = tarfile.DIRTYPE
        root.mode = 0o755
        archive.addfile(root)
        for name, content in [
            ("oci-layout", _json({"imageLayoutVersion": "1.0.0"})),
            ("index.json", index),
            *[("blobs/sha256/" + digest[7:], blob) for digest, blob in blobs.items()],
        ]:
            member = tarfile.TarInfo(name)
            member.mode = 0o644
            member.size = len(content)
            archive.addfile(member, io.BytesIO(content))
    return {
        "manifest": primary_manifest,
        "manifests": manifests,
        "config": config_descriptor,
        "layers": layer_descriptors,
        "diff_ids": actual_diff_ids,
    }


def _working_layers() -> list[tuple[bytes, str]]:
    base = _layer(
        [
            ("etc", None, "dir"),
            ("etc/lower-only", b"remove me\n", "file"),
            ("etc/replaced", b"old\n", "file"),
            ("var", None, "dir"),
            ("var/cache", None, "dir"),
            ("var/cache/lower", b"remove me too\n", "file"),
        ]
    )
    update = _layer(
        [
            ("etc/replaced", b"new\n", "file"),
            ("var/cache/current", b"preserve current layer\n", "file"),
            ("var/cache/.wh..wh..opq", b"", "file"),
            ("etc/.wh.lower-only", b"", "file"),
        ]
    )
    return [(base, OCI_LAYER), (update, OCI_LAYER_GZIP)]


def test_flatten_import_applies_ordered_layers_and_records_provenance(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    image = tmp_path / "image.tar"
    expected = _oci_archive(image, _working_layers(), index_variant="v3")
    root = tmp_path / "root"

    imported = unroot.run(
        "unpack",
        *_ownership_arguments(),
        "--flatten",
        "--platform",
        "linux/x86_64/v3",
        "--inject=-*",
        str(image),
        str(root),
        timeout=90,
    )
    require_capability(
        imported.returncode == 0,
        "OCI flatten import requires rich ID mapping:\n" + imported.diagnostic(),
        "rich_idmap",
    )
    imported.assert_ok()

    assert not (root / "etc" / "lower-only").exists()
    assert (root / "etc" / "replaced").read_text() == "new\n"
    assert not (root / "var" / "cache" / "lower").exists()
    assert (root / "var" / "cache" / "current").read_text() == (
        "preserve current layer\n"
    )
    assert not list(root.rglob(".wh.*"))

    metadata = json.loads((root / ".unroot" / "meta.json").read_text())
    assert re.fullmatch(r"[0-9a-f]{32}", metadata["root_id"])
    assert metadata["oci"]["mode"] == "flattened"
    assert metadata["oci"]["manifest_digest"] == expected["manifest"]["digest"]
    assert metadata["oci"]["config_digest"] == expected["config"]["digest"]
    assert metadata["oci"]["platform"] == "linux/amd64/v3"
    assert metadata["oci"]["image_config"]["Env"] == ["OCI_FIXTURE=yes"]
    assert [item["diff_id"] for item in metadata["oci"]["layers"]] == (
        expected["diff_ids"]
    )

    packed = tmp_path / "flattened.tar"
    unroot.run("pack", str(root), str(packed), timeout=90).assert_ok()
    with tarfile.open(packed) as archive:
        names = {name.removeprefix("./") for name in archive.getnames()}
    assert ".unroot" not in names
    assert "etc/replaced" in names
    assert "etc/lower-only" not in names
    assert not any(Path(name).name.startswith(".wh.") for name in names)


def test_oci_and_raw_archives_require_explicit_matching_modes(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    image = tmp_path / "image.tar"
    _oci_archive(image, _working_layers()[:1])
    no_flatten = unroot.run("unpack", str(image), str(tmp_path / "oci-root"))
    assert no_flatten.returncode != 0
    assert "use unpack --flatten" in no_flatten.stderr

    raw = tmp_path / "raw.tar"
    raw.write_bytes(_working_layers()[0][0])
    flattened_raw = unroot.run(
        "unpack", "--flatten", str(raw), str(tmp_path / "raw-root")
    )
    assert flattened_raw.returncode != 0
    assert "--flatten requires an OCI image-layout archive" in flattened_raw.stderr


def test_oci_import_rejects_wrong_platform_and_corrupt_content(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    image = tmp_path / "image.tar"
    expected = _oci_archive(image, _working_layers()[:1])
    wrong_platform = unroot.run(
        "unpack", "--flatten", "--platform", "linux/arm64",
        str(image), str(tmp_path / "wrong-platform"),
    )
    assert wrong_platform.returncode != 0
    assert "requested platform linux/arm64" in wrong_platform.stderr

    corrupt = tmp_path / "corrupt.tar"
    _oci_archive(
        corrupt,
        _working_layers()[:1],
        corrupt_blob=str(expected["layers"][0]["digest"]),
    )
    corrupted = unroot.run(
        "unpack", "--flatten", str(corrupt), str(tmp_path / "corrupt-root")
    )
    assert corrupted.returncode != 0
    assert "descriptor size does not match" in corrupted.stderr

    wrong_diff = tmp_path / "wrong-diff.tar"
    _oci_archive(
        wrong_diff,
        _working_layers()[:1],
        diff_ids=["sha256:" + "0" * 64],
    )
    mismatched = unroot.run(
        "unpack", "--flatten", str(wrong_diff), str(tmp_path / "diff-root")
    )
    assert mismatched.returncode != 0
    assert "diff_id does not match" in mismatched.stderr

    variant_mismatch = tmp_path / "variant-mismatch.tar"
    _oci_archive(
        variant_mismatch,
        _working_layers()[:1],
        index_variant="v3",
        config_variant="v2",
    )
    mismatched_variant = unroot.run(
        "unpack", "--flatten", str(variant_mismatch),
        str(tmp_path / "variant-root"),
    )
    assert mismatched_variant.returncode != 0
    assert "index platform disagrees" in mismatched_variant.stderr


def test_oci_import_selects_an_explicit_image_from_a_multi_platform_index(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
) -> None:
    image = tmp_path / "multi-platform.tar"
    expected = _oci_archive(
        image,
        _working_layers()[:1],
        additional_platforms=(("arm64", "v8"),),
    )
    root = tmp_path / "root"

    imported = unroot.run(
        "unpack",
        *_ownership_arguments(),
        "--flatten",
        "--platform",
        "linux/arm64/v8",
        "--inject=-*",
        str(image),
        str(root),
        timeout=90,
    )
    require_capability(
        imported.returncode == 0,
        "OCI flatten import requires rich ID mapping:\n" + imported.diagnostic(),
        "rich_idmap",
    )
    imported.assert_ok()

    metadata = json.loads((root / ".unroot" / "meta.json").read_text())
    assert metadata["oci"]["platform"] == "linux/arm64/v8"
    assert metadata["oci"]["manifest_digest"] == (
        expected["manifests"][1]["digest"]
    )


def test_oci_import_rejects_unsupported_runnable_media_types(
    unroot: UnrootRunner, tmp_path: Path
) -> None:
    unknown_manifest = tmp_path / "unknown-manifest.tar"
    _oci_archive(
        unknown_manifest,
        _working_layers()[:1],
        manifest_media_type="application/example.manifest+json",
    )
    manifest_result = unroot.run(
        "unpack", "--flatten", str(unknown_manifest),
        str(tmp_path / "manifest-root"),
    )
    assert manifest_result.returncode != 0
    assert "unsupported manifest media type" in manifest_result.stderr

    unknown_config = tmp_path / "unknown-config.tar"
    _oci_archive(
        unknown_config,
        _working_layers()[:1],
        config_media_type="application/example.config+json",
    )
    config_result = unroot.run(
        "unpack", "--flatten", str(unknown_config),
        str(tmp_path / "config-root"),
    )
    assert config_result.returncode != 0
    assert "unsupported config media type" in config_result.stderr


@pytest.mark.parametrize("root_entry", [".", "./"])
def test_oci_layer_may_contain_a_root_directory_entry(
    unroot: UnrootRunner,
    tmp_path: Path,
    require_capability: Callable[[bool, str, Optional[str]], None],
    root_entry: str,
) -> None:
    """Image builders commonly emit a leading "." entry for the root itself."""
    image = tmp_path / "root-entry.tar"
    layer = _layer(
        [
            (root_entry, None, "dir"),
            ("etc", None, "dir"),
            ("etc/kept", b"yes\n", "file"),
        ]
    )
    _oci_archive(image, [(layer, OCI_LAYER)])
    root = tmp_path / "root"

    imported = unroot.run(
        "unpack",
        *_ownership_arguments(),
        "--flatten",
        "--inject=-*",
        str(image),
        str(root),
        timeout=90,
    )
    assert "unsafe or reserved path" not in imported.stderr, imported.diagnostic()
    require_capability(
        imported.returncode == 0,
        "OCI flatten import requires rich ID mapping:\n" + imported.diagnostic(),
        "rich_idmap",
    )
    imported.assert_ok()
    assert (root / "etc" / "kept").read_text() == "yes\n"


@pytest.mark.parametrize(
    "entry",
    [
        ("safe/../escape", b"bad\n", "file"),
        (".wh.bad", b"target", "symlink"),
        (".wh..unroot", b"", "file"),
    ],
)
def test_oci_layer_safety_is_validated_before_rootfs_initialization(
    unroot: UnrootRunner,
    tmp_path: Path,
    entry: tuple[str, bytes | None, str],
) -> None:
    image = tmp_path / "unsafe.tar"
    _oci_archive(image, [(_layer([entry]), OCI_LAYER)])
    root = tmp_path / "root"

    result = unroot.run("unpack", "--flatten", str(image), str(root))

    assert result.returncode != 0
    assert "unsafe or reserved rootfs paths" in result.stderr
    assert not root.exists()
