"""The saved /dev is archive content, never a live device tree."""

import gzip
import io
import json
import os
import shlex
import shutil
import subprocess
import tarfile
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

from .support import create_rootfs, find_static_busybox
from .test_device_archive import device_archive, rootless_namespace


pytestmark = pytest.mark.e2e


def metadata_entry(path):
    entry = tarfile.TarInfo(path)
    entry.uid, entry.gid = 12345, 23456
    entry.pax_headers = {
        "SCHILY.xattr.user.test": "preserved",
        "SCHILY.acl.access": "user::rw-,user:123:r--,group::r--,mask::r--,other::---",
    }
    return entry


@pytest.mark.parametrize("prefix", ["", "bundle[1]*/root?fs"])
@pytest.mark.parametrize("location", ["inside", "sibling", "wrapper"])
def test_inspection_separates_only_selected_dev(tmp_path, unroot, prefix, location):
    dev = (prefix + "/" if prefix else "") + "dev"
    path = {"inside": dev + "/data", "sibling": dev + "ices/data",
            "wrapper": "wrapper/dev/data"}[location]
    archive = tmp_path / "input.tar"
    with tarfile.open(archive, "w") as output:
        output.addfile(metadata_entry(path))
    args = ["--root-prefix", prefix] if prefix else []
    with archive.open("rb") as stream:
        result = subprocess.run(
            [str(unroot.binary.with_name("unroot-util")), "archive", "inspect",
             "--fd", str(stream.fileno()), *args], pass_fds=(stream.fileno(),),
            capture_output=True, text=True, check=False,
        )
    assert result.returncode == 0, result.stderr
    report = json.loads(result.stdout.split(" ", 1)[1])
    assert report["max_uid"] == 12345 and report["max_gid"] == 23456
    assert report["metadata"]["acls"]["count"] == 1
    assert report["metadata"]["xattrs"]["count"] > 0
    outside = location != "inside"
    assert report["outside_dev"] == {
        "max_uid": 12345 if outside else 0,
        "max_gid": 23456 if outside else 0,
        "posix_acl": outside, "xattr": outside,
    }


@pytest.mark.parametrize("mode", ["preserve", "discard", "oci", "preserve-prefix", "discard-prefix"])
@pytest.mark.parametrize("outside", [False, True])
def test_dev_preflight_exempts_only_unextracted_metadata(
    native_dev_root, tmp_path, unroot, privileged_prefix, mode, outside
):
    from .test_archive import _isolated_unroot, _run_unroot
    from .test_oci_import import _oci_archive, OCI_LAYER

    root, archive, _ = native_dev_root
    prefix = "bundle[1]*/root?fs/" if mode.endswith("-prefix") else ""
    with tarfile.open(archive, "w") as output:
        output.addfile(metadata_entry(prefix + "dev/data"))
        if outside:
            output.addfile(metadata_entry("dev/data" if prefix else "dev-other/data"))
    if mode == "oci":
        image = tmp_path / "oci.tar"
        _oci_archive(image, [(archive.read_bytes(), OCI_LAYER)])
        archive = image
    log = tmp_path / "operations"
    isolated = _isolated_unroot(unroot, tmp_path, f'''
printf '%s %s\\n' "$1" "$2" >> {shlex.quote(str(log))}
if [ "$1" = filesystem ] && [ "$2" = inspect ]; then
    printf '%s\\n' 'unroot-filesystem-v1 {{"posix_acl":{{"detail":"unavailable","supported":false}},"xattr":{{"detail":"unavailable","supported":false}}}}'
    exit 0
fi
''')
    options = {"preserve": [], "discard": ["--discard-dev"], "oci": ["--flatten"]}[mode.split("-")[0]]
    if prefix:
        options += ["--root-prefix", prefix.rstrip("/")]
    result = _run_unroot(isolated, ["unpack", "--native", "--inject=-*", *options,
                                   str(archive), str(root)], {}, privileged_prefix)
    operations = log.read_text().splitlines()
    if outside:
        assert result.returncode != 0
        assert "destination filesystem cannot preserve" in result.stderr
        assert "archive unpack" not in operations and "archive apply-layer" not in operations
    else:
        result.assert_ok()
        assert "filesystem inspect" not in operations
        assert list((root / prefix / "dev").iterdir()) == []
        if mode.startswith("preserve"):
            packed = tmp_path / "packed.tar"
            _run_unroot(isolated, ["pack", str(root), str(packed)], {}, privileged_prefix).assert_ok()
            entry = members(packed)[prefix + "dev/data"]
            assert (entry.uid, entry.gid) == (12345, 23456)
            assert entry.pax_headers["SCHILY.xattr.user.test"] == "preserved"


@pytest.mark.parametrize("path", ["dev/../../escape", "/dev/data", "dev/../../.unroot/meta.json"])
@pytest.mark.parametrize("discard", [False, True])
def test_dev_preflight_still_validates_archive_paths(native_dev_root, path, discard):
    root, archive, run = native_dev_root
    with tarfile.open(archive, "w") as output:
        output.addfile(metadata_entry(path))
    result = run("unpack", "--native", "--inject=-*",
                 *(["--discard-dev"] if discard else []), archive, root, ok=False)
    assert result.returncode != 0
    assert "paths outside the rootfs" in result.stderr or "reserved .unroot" in result.stderr
    assert not root.exists()


@pytest.mark.parametrize("outside", [False, True])
def test_rich_map_checks_only_extracted_ownership(require_rich_idmap, unroot, tmp_path, outside):
    require_rich_idmap(count=1)
    archive = tmp_path / "input.tar"
    with tarfile.open(archive, "w") as output:
        entry = tarfile.TarInfo("payload" if outside else "dev/data")
        entry.uid, entry.gid = 12345, 23456
        output.addfile(entry)
    result = unroot.run("unpack", "--id-count", "1", "--inject=-*",
                        str(archive), str(tmp_path / "root"))
    if outside:
        assert result.returncode != 0
        assert "exceeds the selected rich ID map" in result.stderr
    else:
        result.assert_ok()


def helper_run(helper, prefix, operation, archive, root, dev_path, sidecar=None, *extra):
    mode = "rb" if operation in ("unpack", "apply-layer") else "wb"
    with archive.open(mode) as stream:
        command = [str(helper), "archive", operation, "--fd", str(stream.fileno())]
        if operation == "pack":
            command += ["--filter", "none"]
        command += ["--dev-path", dev_path, *extra]
        descriptors = [stream.fileno()]
        if sidecar:
            descriptors.append(sidecar.fileno())
            command += ["--dev-fd", str(sidecar.fileno())]
        return subprocess.run(
            [*prefix, *command], cwd=root, pass_fds=descriptors,
            capture_output=True, text=True, check=False,
        )


def members(path):
    with tarfile.open(path) as archive:
        return {entry.name.removeprefix("./").rstrip("/"): entry for entry in archive}


@pytest.mark.parametrize("prefix", ["", "bundle[1]*/root?fs/"])
def test_rootless_sidecar_preserves_entries_and_metadata(
    tmp_path, unroot_binary, rootless_namespace, prefix
):
    archive = tmp_path / "input.tar"
    device_archive(archive, True)
    with tarfile.open(archive, "a") as source:
        entry = tarfile.TarInfo("dev/data")
        entry.size, entry.uid, entry.gid, entry.mode = 7, 12345, 23456, 0o640
        entry.pax_headers = {
            "mtime": "1234567890.123456789",
            "SCHILY.xattr.user.test": "preserved",
            "SCHILY.acl.access": "user::rw-,user:123:r--,group::r--,mask::r--,other::---",
        }
        source.addfile(entry, io.BytesIO(b"payload"))
        for name, kind, target in (
            ("hard", tarfile.LNKTYPE, "dev/data"),
            ("sym", tarfile.SYMTYPE, "../etc/passwd"),
            ("fifo", tarfile.FIFOTYPE, ""),
        ):
            entry = tarfile.TarInfo("dev/" + name)
            entry.type, entry.linkname = kind, target
            source.addfile(entry)
    if prefix:
        wrapped = tmp_path / "wrapped.tar"
        with tarfile.open(archive) as source, tarfile.open(wrapped, "w") as output:
            for entry in source:
                content = source.extractfile(entry) if entry.isreg() else None
                entry.name = prefix + entry.name
                if entry.islnk():
                    entry.linkname = prefix + entry.linkname
                output.addfile(entry, content)
        archive = wrapped
    root = tmp_path / "root"
    root.mkdir()
    snapshot = tmp_path / "dev.tar"
    helper = unroot_binary.with_name("unroot-util")
    with snapshot.open("wb") as saved:
        result = helper_run(helper, rootless_namespace, "unpack", archive, root,
                            prefix + "dev", saved)
    assert result.returncode == 0, result.stderr
    dev = root / prefix / "dev"
    assert list(dev.iterdir()) == []
    (dev / "not-original").write_text("must not be packed")
    captured = tmp_path / "output.tar"
    with snapshot.open("rb") as saved:
        result = helper_run(helper, rootless_namespace, "pack", captured, root,
                            prefix + "dev", saved)
    assert result.returncode == 0, result.stderr
    before, after = members(archive), members(captured)
    assert {name for name in after if name.startswith(prefix + "dev")} == set(before)
    for name, original in before.items():
        restored = after[name]
        for field in ("type", "devmajor", "devminor", "uid", "gid", "mode", "linkname"):
            assert getattr(restored, field) == getattr(original, field), (name, field)
    data = after[prefix + "dev/data"]
    assert data.pax_headers["mtime"] == "1234567890.123456789"
    assert data.pax_headers["SCHILY.xattr.user.test"] == "preserved"
    assert "123" in data.pax_headers["SCHILY.acl.access"]
    with tarfile.open(captured) as output:
        assert output.extractfile(prefix + "dev/data").read() == b"payload"


@pytest.mark.parametrize("operation", ["unpack", "apply-layer"])
def test_discard_dev_never_creates_or_packs_nodes(
    tmp_path, unroot_binary, rootless_namespace, operation
):
    archive = tmp_path / "input.tar"
    device_archive(archive, True)
    root = tmp_path / "root"
    root.mkdir()
    helper = unroot_binary.with_name("unroot-util")
    result = helper_run(helper, rootless_namespace, operation, archive, root, "dev")
    assert result.returncode == 0, result.stderr
    assert list((root / "dev").iterdir()) == []
    (root / "dev/unwanted").write_text("not original")
    captured = tmp_path / "output.tar"
    result = helper_run(helper, rootless_namespace, "pack", captured, root, "dev")
    assert result.returncode == 0, result.stderr
    assert members(captured)["dev"].isdir()
    assert not any(name.startswith("dev/") for name in members(captured))


@pytest.mark.parametrize("rule", ["dev/null", "dev", "dev/upstream-*"])
def test_sidecar_obeys_exclusions(tmp_path, unroot_binary, rootless_namespace, rule):
    snapshot = tmp_path / "dev.tar"
    device_archive(snapshot, True)
    root = tmp_path / "root"
    root.mkdir()
    output = tmp_path / "output.tar"
    with snapshot.open("rb") as saved:
        result = helper_run(unroot_binary.with_name("unroot-util"), rootless_namespace,
                            "pack", output, root, "dev", saved, "--exclude-glob", rule)
    assert result.returncode == 0, result.stderr
    names = members(output)
    if rule == "dev":
        assert not any(name == "dev" or name.startswith("dev/") for name in names)
    elif rule == "dev/null":
        assert "dev/null" not in names and "dev/console" in names
    else:
        assert "dev/upstream-marker" not in names and "dev/null" in names


@pytest.mark.parametrize("path", ["../outside", "dev/../outside", "/dev/null", "etc/passwd"])
def test_pack_rejects_unsafe_sidecar_entries(tmp_path, unroot_binary, rootless_namespace, path):
    snapshot = tmp_path / "dev.tar"
    with tarfile.open(snapshot, "w") as archive:
        archive.addfile(tarfile.TarInfo(path))
    root = tmp_path / "root"
    root.mkdir()
    with snapshot.open("rb") as saved:
        result = helper_run(unroot_binary.with_name("unroot-util"), rootless_namespace,
                            "pack", tmp_path / "output.tar", root, "dev", saved)
    assert result.returncode != 0
    assert "invalid path" in result.stderr


@pytest.mark.parametrize("inside_first", [False, True])
@pytest.mark.parametrize("discard", [False, True])
@pytest.mark.parametrize("prefix", ["", "bundle/rootfs/"])
def test_unpack_rejects_links_across_dev_boundary(
    tmp_path, unroot_binary, rootless_namespace, inside_first, discard, prefix
):
    archive = tmp_path / "input.tar"
    names = [prefix + "dev/original", prefix + "ordinary"]
    if not inside_first:
        names.reverse()
    with tarfile.open(archive, "w") as output:
        output.addfile(tarfile.TarInfo(names[0]))
        entry = tarfile.TarInfo(names[1])
        entry.type, entry.linkname = tarfile.LNKTYPE, names[0]
        output.addfile(entry)
    root = tmp_path / "root"
    root.mkdir()
    with (tmp_path / "dev.tar").open("wb") as saved:
        result = helper_run(unroot_binary.with_name("unroot-util"), rootless_namespace,
                            "unpack", archive, root, prefix + "dev",
                            None if discard else saved, "--force")
    assert result.returncode != 0
    assert "hard link crosses" in result.stderr


@pytest.mark.parametrize("format", ["newc", "crc", "odc", "bin"])
@pytest.mark.parametrize("payload", [b"", b"preserved contents"])
@pytest.mark.parametrize("compressed", [False, True])
@pytest.mark.parametrize("partial", [False, True])
def test_cpio_dev_hardlinks_preserve_payload(
    tmp_path, unroot_binary, rootless_namespace, format, payload, compressed, partial
):
    cpio = shutil.which("cpio")
    if not cpio:
        pytest.skip("cpio is required to generate the input fixture")
    source = tmp_path / "source"
    (source / "dev").mkdir(parents=True)
    (source / "dev/original").write_bytes(payload)
    (source / "dev/original").chmod(0o640)
    os.utime(source / "dev/original", (1234567890, 1234567890))
    os.link(source / "dev/original", source / "dev/linked")
    os.link(source / "dev/original", source / "dev/third")
    # A link count can include names not present in the archive.
    if partial:
        os.link(source / "dev/original", source / "unarchived")
    (source / "dev/unrelated").write_bytes(b"unrelated")
    archive = tmp_path / "input.cpio"
    with archive.open("wb") as output:
        generated = subprocess.run(
            [cpio, "--create", "--format=" + format, "--owner=0:0", "--quiet"], cwd=source,
            input=b"dev\ndev/original\ndev/unrelated\ndev/linked\ndev/third\n", stdout=output,
            stderr=subprocess.PIPE, check=False,
        )
    assert generated.returncode == 0, generated.stderr
    if compressed:
        archive.write_bytes(gzip.compress(archive.read_bytes()))
    root = tmp_path / "root"
    root.mkdir()
    snapshot = tmp_path / "dev.tar"
    packed = tmp_path / "output.tar"
    helper = unroot_binary.with_name("unroot-util")
    for operation, path, mode in (("unpack", archive, "wb"), ("pack", packed, "rb")):
        with snapshot.open(mode) as saved:
            result = helper_run(helper, rootless_namespace, operation, path, root, "dev", saved)
        assert result.returncode == 0, result.stderr
    with tarfile.open(packed) as output:
        assert output.extractfile("dev/unrelated").read() == b"unrelated"
        linked = [output.getmember(name) for name in ("dev/original", "dev/linked", "dev/third")]
        regular = [entry.name for entry in linked if entry.isreg()]
        assert len(regular) == 1
        for name in ("dev/original", "dev/linked", "dev/third"):
            assert output.extractfile(name).read() == payload
            entry = output.getmember(name)
            assert (entry.mode, entry.mtime) == (0o640, 1234567890)
            assert (entry.uid, entry.gid) == (0, 0)
            if name != regular[0]:
                assert entry.islnk() and entry.linkname == regular[0]


@pytest.mark.parametrize("inside_first", [False, True])
@pytest.mark.parametrize("discard", [False, True])
def test_cpio_rejects_links_across_dev_boundary(
    tmp_path, unroot_binary, rootless_namespace, inside_first, discard
):
    cpio = shutil.which("cpio")
    if not cpio:
        pytest.skip("cpio is required to generate the input fixture")
    source = tmp_path / "source"
    (source / "dev").mkdir(parents=True)
    (source / "ordinary").write_bytes(b"shared contents")
    os.link(source / "ordinary", source / "dev/linked")
    names = b"dev/linked\nordinary\n" if inside_first else b"ordinary\ndev/linked\n"
    archive = tmp_path / "input.cpio"
    with archive.open("wb") as output:
        generated = subprocess.run(
            [cpio, "--create", "--format=newc", "--owner=0:0", "--quiet"], cwd=source,
            input=names, stdout=output, stderr=subprocess.PIPE, check=False,
        )
    assert generated.returncode == 0, generated.stderr
    root = tmp_path / "root"
    root.mkdir()
    with (tmp_path / "dev.tar").open("wb") as saved:
        result = helper_run(unroot_binary.with_name("unroot-util"), rootless_namespace,
                            "unpack", archive, root, "dev", None if discard else saved)
    assert result.returncode != 0
    assert "hard link crosses" in result.stderr


@pytest.fixture
def native_dev_root(tmp_path, unroot_binary, privileged_prefix):
    busybox = find_static_busybox()
    if not busybox:
        pytest.skip("static BusyBox required")
    source = create_rootfs(tmp_path / "source", busybox)
    (source / "dev/upstream-marker").write_text("original")
    archive = tmp_path / "source.tar"
    def root_owner(entry):
        entry.uid = entry.gid = 0
        return entry
    with tarfile.open(archive, "w") as output:
        output.add(source, arcname=".", filter=root_owner)
        node = tarfile.TarInfo("dev/null")
        node.type, node.devmajor, node.devminor = tarfile.CHRTYPE, 1, 3
        node.mode = 0o666
        output.addfile(node)
    root = tmp_path / "root"
    command = [*privileged_prefix, str(unroot_binary)]
    def run(*arguments, ok=True):
        result = subprocess.run([*command, *map(str, arguments)], capture_output=True,
                                text=True, check=False, timeout=30)
        if ok:
            assert result.returncode == 0, result.stdout + result.stderr
        return result
    try:
        yield root, archive, run
    finally:
        if root.exists():
            subprocess.run([*privileged_prefix, "rm", "-rf", "--", str(root)], check=True)


@pytest.mark.parametrize("discard", [False, True])
@pytest.mark.parametrize("while_entered", [False, True])
def test_managed_dev_lifecycle(native_dev_root, tmp_path, discard, while_entered):
    root, archive, run = native_dev_root
    run("unpack", "--native", "--inject=-*", *(["--discard-dev"] if discard else []), archive, root)
    assert list((root / "dev").iterdir()) == []
    entered = "test -c /dev/null && test ! -e /dev/upstream-marker && echo runtime > /dev/session"
    with ThreadPoolExecutor(max_workers=1) as executor:
        if while_entered:
            future = executor.submit(run, "enter", root, "--", "/bin/sh", "-c",
                entered + " && /bin/busybox touch /ready; "
                "while test ! -e /release; do /bin/busybox sleep 0.05; done")
            deadline = time.monotonic() + 10
            while not (root / "ready").exists() and not future.done() and time.monotonic() < deadline:
                time.sleep(0.05)
        else:
            run("enter", root, "--", "/bin/sh", "-c", entered)
        try:
            if while_entered:
                assert (root / "ready").exists(), "enter did not provide working devices"
            assert list((root / "dev").iterdir()) == []
            output = tmp_path / "output.tar"
            run("pack", root, output)
            contents = members(output)
            assert "dev/session" not in contents
            if discard:
                assert contents["dev"].isdir()
                assert not any(name.startswith("dev/") for name in contents)
            else:
                assert contents["dev/null"].ischr()
                assert "dev/upstream-marker" in contents
        finally:
            if while_entered:
                run("enter", root, "--", "/bin/busybox", "touch", "/release")
                future.result()


@pytest.mark.parametrize("damage", ["missing", "corrupt", "symlink", "invalid-policy"])
def test_pack_fails_closed_for_damaged_preservation(
    native_dev_root, tmp_path, privileged_prefix, damage
):
    root, archive, run = native_dev_root
    run("unpack", "--native", "--inject=-*", archive, root)
    program = (
        "import json,pathlib,sys; p=pathlib.Path(sys.argv[1]); kind=sys.argv[2]; "
        "s=p/'.unroot/dev.tar'; m=p/'.unroot/meta.json'; "
        "s.unlink() if kind in ('missing','symlink') else None; "
        "s.symlink_to('/dev/null') if kind=='symlink' else None; "
        "s.write_bytes(b'broken') if kind=='corrupt' else None; "
        "m.write_text(json.dumps({**json.loads(m.read_text()),'dev_policy':'bogus'})) "
        "if kind=='invalid-policy' else None"
    )
    subprocess.run([*privileged_prefix, "python3", "-c", program, str(root), damage], check=True)
    output = tmp_path / "output.tar"
    output.write_bytes(b"previous archive")
    result = run("pack", "--force", root, output, ok=False)
    assert result.returncode != 0
    assert output.read_bytes() == b"previous archive"


def test_existing_root_preserves_underlying_dev(tmp_path, unroot, rootless_namespace):
    busybox = find_static_busybox()
    if not busybox:
        pytest.skip("static BusyBox required")
    root = create_rootfs(tmp_path / "existing", busybox)
    (root / "dev/null").write_text("upstream regular file")
    (root / "dev/marker").write_text("original")
    mapped = tmp_path / "mapped"
    mapped.write_text("mapped file")
    unroot.run("enter", "--single", "--map-ro", f"{mapped}:/dev/custom", str(root),
               "--", "/bin/sh", "-c",
               "test -c /dev/null && test ! -e /dev/marker && "
               "test -f /dev/custom && echo ephemeral > /dev/session").assert_ok()
    assert {path.name for path in (root / "dev").iterdir()} == {"null", "marker"}
    output = tmp_path / "output.tar"
    unroot.run("pack", "--single", str(root), str(output)).assert_ok()
    with tarfile.open(output) as packed:
        assert packed.extractfile("./dev/null").read() == b"upstream regular file"


def test_rich_unpack_preserves_device_archive(require_rich_idmap, unroot, tmp_path):
    require_rich_idmap()
    archive = tmp_path / "input.tar"
    device_archive(archive, True)
    root = tmp_path / "rich"
    unroot.run("unpack", "--inject=-*", str(archive), str(root)).assert_ok()
    assert list((root / "dev").iterdir()) == []
    output = tmp_path / "output.tar"
    unroot.run("pack", str(root), str(output)).assert_ok()
    assert members(output)["dev/null"].ischr()


@pytest.mark.parametrize("prefix", ["", "bundle[1]*/root?fs"])
def test_full_cli_rootless_preservation(tmp_path, unroot_binary, rootless_namespace, prefix):
    # Host-unprivileged even though --native runs as UID 0 inside this outer
    # user namespace. The separate rich test exercises allocated subuids.
    original = tmp_path / "original.tar"
    device_archive(original, True)
    archive = original
    if prefix:
        archive = tmp_path / "wrapped.tar"
        with tarfile.open(original) as source, tarfile.open(archive, "w") as target:
            for entry in source:
                content = source.extractfile(entry) if entry.isreg() else None
                entry.name = prefix + "/" + entry.name
                target.addfile(entry, content)
            outside = tarfile.TarInfo("dev/outside-root")
            outside.size = 7
            target.addfile(outside, io.BytesIO(b"wrapper"))
    root, output = tmp_path / "root", tmp_path / "output.tar"
    for arguments in (
        ["unpack", "--native", "--inject=-*", *(["--root-prefix", prefix] if prefix else []),
         str(archive), str(root)],
        ["pack", str(root), str(output)],
    ):
        result = subprocess.run([*rootless_namespace, str(unroot_binary), *arguments],
                                text=True, capture_output=True, check=False)
        assert result.returncode == 0, result.stderr
    assert list((root / prefix / "dev").iterdir()) == []
    assert members(output)[(prefix + "/" if prefix else "") + "dev/null"].ischr()
    if prefix:
        assert (root / "dev/outside-root").read_bytes() == b"wrapper"
        assert "dev/outside-root" in members(output)


def test_oci_import_discards_original_dev(native_dev_root, tmp_path):
    from .test_oci_import import _oci_archive, _layer, OCI_LAYER

    root, _, run = native_dev_root
    device_layer = tmp_path / "layer.tar"
    device_archive(device_layer, True)
    image = tmp_path / "oci.tar"
    _oci_archive(image, [
        (device_layer.read_bytes(), OCI_LAYER),
        (_layer([("dev/.wh.null", b"", "file"),
                 ("dev/new-device-file", b"discard", "file"),
                 ("payload", b"keep", "file")]), OCI_LAYER),
    ])
    run("unpack", "--native", "--flatten", "--inject=-*", image, root)
    assert list((root / "dev").iterdir()) == []
    assert (root / "payload").read_bytes() == b"keep"
    output = tmp_path / "packed.tar"
    run("pack", root, output)
    assert members(output)["dev"].isdir()
    assert not any(name.startswith("dev/") for name in members(output))


def test_exclude_action_applies_to_saved_dev(native_dev_root, tmp_path):
    root, archive, run = native_dev_root
    run("unpack", "--native", "--inject=-*", archive, root)
    run("exclude", "add", root, "/dev/null")
    output = tmp_path / "packed.tar"
    run("pack", root, output)
    assert "dev/null" not in members(output)
    assert "dev/upstream-marker" in members(output)


def test_empty_oci_image_gets_empty_dev(native_dev_root, tmp_path):
    from .test_oci_import import _oci_archive

    root, _, run = native_dev_root
    image = tmp_path / "empty-oci.tar"
    _oci_archive(image, [])
    run("unpack", "--native", "--flatten", "--inject=-*", image, root)
    assert list((root / "dev").iterdir()) == []
    output = tmp_path / "packed.tar"
    run("pack", root, output)
    assert members(output)["dev"].isdir()
