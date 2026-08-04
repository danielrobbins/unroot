#!/usr/bin/env python3
"""Compile Unroot's Docatoms source into checked publication artifacts."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "unroot.docatoms"
OUTPUTS = {
    ROOT / "docs" / "unroot.docs.json": "json",
    ROOT / "docs" / "unroot.md": "markdown",
    ROOT / "docs" / "unroot.1": "man",
}

sys.path.insert(0, str(ROOT))

from tools.docatoms import parse, render_man, render_markdown  # noqa: E402


def build(source: Path, version: str) -> dict[Path, str]:
    document = parse(source.read_text(encoding="utf-8"))
    return {
        path: {
            "json": document.render_json,
            "markdown": lambda: render_markdown(document),
            "man": lambda: render_man(document, version),
        }[kind]()
        for path, kind in OUTPUTS.items()
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build Unroot documentation artifacts.")
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--version", default=(ROOT / "VERSION").read_text(encoding="ascii").strip())
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    rendered = build(args.source, args.version)
    stale = [
        path
        for path, content in rendered.items()
        if not path.is_file() or path.read_text(encoding="utf-8") != content
    ]
    if args.check:
        if stale:
            sys.stderr.write("generated documentation is stale: " + ", ".join(str(path) for path in stale) + "\n")
            return 1
        return 0

    for path, content in rendered.items():
        with path.open("w", encoding="utf-8", newline="\n") as output:
            output.write(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
