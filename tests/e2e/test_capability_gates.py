from __future__ import annotations

import os
from pathlib import Path

import pytest

from . import conftest, support, test_oci_import
from .support import CommandResult


class FailedOperation:
    def run(self, *arguments, **kwargs):
        return CommandResult(("unroot", *arguments), 1, "", "deliberate product failure")


def test_managed_rootfs_failure_is_not_a_capability_skip(tmp_path, monkeypatch):
    monkeypatch.setattr(conftest.subprocess, "run", lambda *args, **kwargs: None)
    require = lambda: (100000, 100000)
    try:
        with pytest.raises(AssertionError, match="deliberate product failure"):
            conftest._managed_rootfs(tmp_path, FailedOperation(), require)
    except pytest.skip.Exception:
        pytest.fail("managed rootfs failure was hidden by a capability skip")


@pytest.mark.parametrize("scenario", [
    test_oci_import.test_flatten_import_applies_ordered_layers_and_records_provenance,
    test_oci_import.test_oci_import_selects_an_explicit_image_from_a_multi_platform_index,
])
def test_oci_failure_is_not_a_capability_skip(tmp_path, monkeypatch, scenario):
    monkeypatch.setattr(os, "geteuid", lambda: 1000)
    # Qualified environment: the setup check succeeds, but the real import fails.
    def require(*args):
        if args and args[0] is False:
            pytest.skip("operation incorrectly treated as an unavailable capability")
    try:
        with pytest.raises(AssertionError, match="deliberate product failure"):
            scenario(FailedOperation(), tmp_path, require)
    except pytest.skip.Exception:
        pytest.fail("OCI import failure was hidden by a capability skip")


@pytest.mark.parametrize("strict, required, outcome", [
    (False, set(), pytest.skip.Exception),
    (True, {"rich_idmap"}, pytest.fail.Exception),
    (True, {"cross_arch"}, pytest.skip.Exception),
])
def test_missing_capability_obeys_the_requested_ci_policy(strict, required, outcome):
    require = conftest.require_capability.__wrapped__(strict, required)
    with pytest.raises(outcome, match="subordinate IDs unavailable"):
        require(False, "subordinate IDs unavailable", "rich_idmap")


@pytest.fixture
def helper(tmp_path):
    path = tmp_path / "unroot-util"
    path.write_text("test fixture")
    path.chmod(0o700)
    return path


@pytest.mark.parametrize("message", [
    "error: no subordinate UID ranges available for tester",
    "error: no subordinate GID ranges available for tester",
    "error: no subordinate UID range can map 65535 IDs",
    "error: no subordinate GID range can map 65535 IDs",
])
def test_known_missing_allocation_can_skip(helper, monkeypatch, message):
    monkeypatch.setattr(support, "run_command", lambda _: CommandResult(
        (str(helper),), 1, "", message
    ))
    assert support.rich_idmap_allocation(helper, 65535) == (None, message)


@pytest.mark.parametrize("code, output, error", [
    (-11, "", ""),
    (1, "", "unexpected helper failure"),
    (0, "unrecognized response", ""),
    (0, "unroot-idmap-v1 100000 100000 1 files", ""),
    (0, "unroot-idmap-v1 invalid 100000 65535 files", ""),
])
def test_broken_helper_is_not_an_unavailable_environment(
    helper, monkeypatch, code, output, error,
):
    monkeypatch.setattr(support, "run_command", lambda _: CommandResult(
        (str(helper),), code, output, error
    ))
    with pytest.raises((AssertionError, ValueError)):
        support.rich_idmap_allocation(helper, 65535)


def test_missing_built_helper_is_a_failure(tmp_path):
    with pytest.raises(AssertionError, match="not found"):
        support.rich_idmap_allocation(tmp_path / "missing-util", 65535)


@pytest.mark.parametrize("message", [
    "unshare: unshare failed: Operation not permitted",
    "unshare: unshare failed: Permission denied",
    "unshare: unshare failed: No space left on device",
    "newuidmap: write to uid_map failed: Operation not permitted",
    "newgidmap: write to gid_map failed: Operation not permitted",
])
def test_known_namespace_restriction_can_skip(monkeypatch, message):
    monkeypatch.setattr(support.shutil, "which", lambda name: "/usr/bin/" + name)
    monkeypatch.setattr(support, "run_command", lambda _: CommandResult(
        ("unshare",), 1, "", message
    ))
    assert message in support.rich_namespace_unavailable((100000, 200000), 65535)


def test_unexpected_namespace_probe_failure_is_not_a_skip(monkeypatch):
    monkeypatch.setattr(support.shutil, "which", lambda name: "/usr/bin/" + name)
    monkeypatch.setattr(support, "run_command", lambda _: CommandResult(
        ("unshare",), 1, "", "unrecognized option"
    ))
    with pytest.raises(AssertionError, match="unrecognized option"):
        support.rich_namespace_unavailable((100000, 200000), 65535)


def test_namespace_probe_exercises_the_allocated_rich_map(monkeypatch):
    monkeypatch.setattr(support.shutil, "which", lambda name: "/usr/bin/" + name)
    calls = []
    def run(command):
        calls.append(command)
        return CommandResult(tuple(command), 0, "", "")
    monkeypatch.setattr(support, "run_command", run)
    assert support.rich_namespace_unavailable((100000, 200000), 42) == ""
    assert "--map-users=100000,0,42" in calls[0]
    assert "--map-groups=200000,0,42" in calls[0]
    assert "--map-root-user" not in calls[0]


def test_qualification_is_reused_and_supports_custom_id_counts(monkeypatch):
    calls = []
    def allocation(helper, count):
        calls.append(count)
        return (100000, 200000), ""
    monkeypatch.setattr(conftest, "rich_idmap_allocation", allocation)
    monkeypatch.setattr(conftest, "rich_namespace_unavailable", lambda allocation, count: "")
    require = conftest.require_rich_idmap.__wrapped__(
        Path("/bin/unroot"), conftest.require_capability.__wrapped__(False, set())
    )
    assert require() == (100000, 200000)
    assert require(capability="rootfs_journey") == (100000, 200000)
    assert require(100000) == (100000, 200000)
    assert calls == [65535, 100000]


def test_native_oci_import_does_not_require_subordinate_ids(tmp_path, monkeypatch):
    monkeypatch.setattr(os, "geteuid", lambda: 0)
    def not_called(*args):
        pytest.fail("native OCI import must not require rich IDs")
    with pytest.raises(AssertionError, match="deliberate product failure"):
        test_oci_import.test_flatten_import_applies_ordered_layers_and_records_provenance(
            FailedOperation(), tmp_path, not_called
        )
