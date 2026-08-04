#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_PATTERN = re.compile(
    r"^[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:_[A-Za-z0-9]+)?$"
)
ARCHITECTURES = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "aarch64": "arm64",
    "arm64": "arm64",
}


@dataclass(frozen=True)
class Release:
    version: str
    title: str
    prerelease: bool


def release_metadata(version: str, tag: str | None = None) -> Release:
    version = version.strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"unsupported VERSION format: {version}")
    if tag is not None and tag != version:
        raise ValueError(f"tag {tag} does not match VERSION {version}")
    return Release(version, version.replace("_", " "), "_" in version)


def current_release(tag: str | None = None) -> Release:
    return release_metadata((ROOT / "VERSION").read_text(encoding="utf-8"), tag)


def normalized_architecture(machine: str) -> str:
    try:
        return ARCHITECTURES[machine.lower()]
    except KeyError as error:
        raise ValueError(f"unsupported build architecture: {machine}") from error


def extract_notes(text: str, version: str) -> str:
    heading = f"# Unroot {version}"
    lines = text.splitlines()
    try:
        start = lines.index(heading) + 1
    except ValueError as error:
        raise ValueError(f"release notes have no {heading!r} section") from error
    end = next(
        (offset for offset in range(start, len(lines)) if lines[offset].startswith("# Unroot ")),
        len(lines),
    )
    notes = "\n".join(lines[start:end]).strip()
    if not notes:
        raise ValueError(f"release notes for {version} are empty")
    return notes + "\n"


def write_checksums(directory: Path, release: Release, required: set[str]) -> Path:
    prefix = f"unroot-{release.version}-linux-"
    standalone = sorted(directory.glob(prefix + "*"))
    architectures = {artifact.name.removeprefix(prefix) for artifact in standalone}
    if not required.issubset(architectures):
        raise ValueError(
            "release architectures do not match: "
            f"required {sorted(required)}, found {sorted(architectures)}"
        )
    artifacts = sorted(
        artifact
        for artifact in directory.iterdir()
        if artifact.is_file()
        and artifact.name not in {"release-notes.md", "SHA256SUMS"}
    )
    if not artifacts:
        raise ValueError("release contains no artifacts")
    output = directory / "SHA256SUMS"
    lines = [
        f"{hashlib.sha256(artifact.read_bytes()).hexdigest()}  {artifact.name}"
        for artifact in artifacts
    ]
    output.write_text("\n".join(lines) + "\n", encoding="ascii")
    return output


def build_artifact(output_dir: Path, architecture: str) -> Path:
    release = current_release()
    host = normalized_architecture(platform.machine())
    if architecture != host:
        raise ValueError(
            f"requested {architecture} artifact on {host} build host"
        )
    subprocess.run(["make", "clean"], cwd=ROOT, check=True)
    subprocess.run(["make", "cli"], cwd=ROOT, check=True)
    binary = ROOT / "bin" / "unroot"
    reported = subprocess.run(
        [binary, "--version"], check=True, text=True, capture_output=True
    ).stdout.strip()
    if reported != release.version:
        raise ValueError(
            f"built binary reports {reported}, expected {release.version}"
        )
    elf = subprocess.run(
        ["readelf", "-lW", binary], check=True, text=True, capture_output=True
    ).stdout
    if " INTERP " in elf:
        raise ValueError("release binary contains a dynamic interpreter")
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact = output_dir / f"unroot-{release.version}-linux-{architecture}"
    shutil.copy2(binary, artifact)
    artifact.chmod(0o755)
    return artifact


def write_github_output(path: Path, release: Release) -> None:
    with path.open("a", encoding="utf-8") as output:
        output.write(f"version={release.version}\n")
        output.write(f"title={release.title}\n")
        output.write(f"prerelease={str(release.prerelease).lower()}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build and assemble Unroot releases.")
    commands = parser.add_subparsers(dest="command", required=True)

    metadata = commands.add_parser("metadata", help="validate release identity")
    metadata.add_argument("--tag", default=os.environ.get("GITHUB_REF_NAME"))
    metadata.add_argument("--github-output", type=Path)

    notes = commands.add_parser("notes", help="extract notes for VERSION")
    notes.add_argument("output", type=Path)

    build = commands.add_parser("build", help="build one native static artifact")
    build.add_argument("--architecture", required=True, choices={"x86_64", "arm64"})
    build.add_argument("--output-dir", type=Path, default=ROOT / "dist")

    checksums = commands.add_parser("checksums", help="hash assembled artifacts")
    checksums.add_argument("directory", type=Path)
    checksums.add_argument(
        "--require-architecture",
        action="append",
        choices={"x86_64", "arm64"},
        required=True,
    )

    args = parser.parse_args()
    if args.command == "metadata":
        release = current_release(args.tag)
        if args.github_output:
            write_github_output(args.github_output, release)
        print(json.dumps(release.__dict__, sort_keys=True))
    elif args.command == "notes":
        release = current_release()
        text = (ROOT / "RELEASE_NOTES.md").read_text(encoding="utf-8")
        args.output.write_text(
            extract_notes(text, release.version), encoding="utf-8"
        )
    elif args.command == "build":
        print(build_artifact(args.output_dir, args.architecture))
    else:
        release = current_release()
        print(
            write_checksums(
                args.directory, release, set(args.require_architecture)
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
