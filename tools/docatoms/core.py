"""Parse tagged documentation atoms and compile their document outline."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

SCHEMA = "docatoms/v1"
_HEADING_RE = re.compile(r"^==\s+@([a-z][a-z0-9-]*)(?:\s+(.*\S))?\s*$")
_METADATA_RE = re.compile(r"^(.*?)\s*\[(.*?)\]\s*$")


@dataclass
class Atom:
    lineno: int
    kind: str
    name: str
    short_help: str = ""
    syntax: str = ""
    body: str = ""
    metadata: dict[str, str] = field(default_factory=dict)

    @property
    def tag(self) -> str:
        return f"{self.kind}:{self.name}"

    @property
    def parent(self) -> str | None:
        return self.metadata.get("parent")

    @property
    def priority(self) -> int:
        value = self.metadata.get("priority", "50")
        try:
            return int(value)
        except ValueError as exc:
            raise ValueError(f"line {self.lineno}: invalid priority {value!r} for {self.tag}") from exc

    @property
    def title(self) -> str:
        return self.metadata.get("title", self.short_help or self.name)

    def json_record(self) -> dict[str, object]:
        return {
            "kind": self.kind,
            "name": self.name,
            "title": self.title,
            "short_help": self.short_help,
            "syntax": self.syntax,
            "description": self.body,
            "metadata": self.metadata,
        }


class Document:
    def __init__(self, atoms: list[Atom]):
        self.atoms = tuple(atoms)
        self.by_tag = {atom.tag: atom for atom in atoms}
        self._validate()

    def _validate(self) -> None:
        seen: set[str] = set()
        for atom in self.atoms:
            if atom.tag in seen:
                raise ValueError(f"duplicate doc tag {atom.tag}")
            seen.add(atom.tag)

        tools = [atom for atom in self.atoms if atom.kind == "tool"]
        if len(tools) != 1:
            raise ValueError(f"expected exactly one @tool atom, found {len(tools)}")
        if tools[0].parent:
            raise ValueError(f"line {tools[0].lineno}: @tool atom cannot have a parent")

        for atom in self.atoms:
            atom.priority
            if atom.parent and atom.parent not in self.by_tag:
                raise ValueError(f"line {atom.lineno}: unknown parent {atom.parent!r} for {atom.tag}")

            chain: set[str] = {atom.tag}
            parent = atom.parent
            while parent:
                if parent in chain:
                    raise ValueError(f"line {atom.lineno}: parent cycle involving {atom.tag}")
                chain.add(parent)
                parent = self.by_tag[parent].parent

        if self.children(None)[0] is not tools[0]:
            raise ValueError(f"line {tools[0].lineno}: @tool atom must be the first root entry")

    def children(self, parent: str | None) -> list[Atom]:
        return sorted(
            (atom for atom in self.atoms if atom.parent == parent),
            key=lambda atom: (atom.priority, atom.title.casefold(), atom.tag),
        )

    def _outline(self, parent: str | None = None) -> list[dict[str, object]]:
        return [
            {"tag": atom.tag, "children": self._outline(atom.tag)}
            for atom in self.children(parent)
        ]

    def render_json(self) -> str:
        payload = {
            "schema": SCHEMA,
            "atoms": {atom.tag: atom.json_record() for atom in self.atoms},
            "outline": self._outline(),
        }
        return json.dumps(payload, indent=2, ensure_ascii=False) + "\n"


def _split_heading(rest: str) -> tuple[str, str, dict[str, str]]:
    name, _, short_help = rest.partition(":")
    match = _METADATA_RE.match(short_help)
    if not match:
        return name.rstrip(), short_help.strip(), {}

    metadata: dict[str, str] = {}
    for item in match.group(2).split(","):
        key, separator, value = item.partition("=")
        if not separator or not key.strip() or not value.strip():
            raise ValueError(f"invalid metadata item {item!r}")
        metadata[key.strip()] = value.strip()
    return name.rstrip(), match.group(1).strip(), metadata


def parse(text: str) -> Document:
    atoms: list[Atom] = []
    current: Atom | None = None
    body: list[str] = []

    def finish(lineno: int) -> None:
        nonlocal current, body
        if current is None:
            return
        current.body = "".join(body).rstrip("\r\n")
        if current.kind not in {"section"} and not current.body.strip():
            raise ValueError(f"line {lineno}: empty doc body for {current.tag}")
        atoms.append(current)
        current = None
        body = []

    for lineno, line in enumerate(text.splitlines(keepends=True), start=1):
        if line.lstrip().startswith("%% @"):
            raise ValueError(f"line {lineno}: pseudo-heading must use == @")
        heading = _HEADING_RE.match(line.rstrip("\r\n"))
        if heading:
            finish(lineno)
            kind, rest = heading.groups()
            try:
                name, short_help, metadata = _split_heading(rest or "")
            except ValueError as exc:
                raise ValueError(f"line {lineno}: {exc}") from exc
            if not name:
                raise ValueError(f"line {lineno}: @{kind} requires a name")
            current = Atom(lineno, kind, name, short_help, metadata=metadata)
            continue

        if current is None:
            raise ValueError(f"line {lineno}: text outside a tagged block: {line!r}")

        stripped = line.strip()
        body_started = any(part.strip() for part in body)
        if not body_started and stripped.startswith("@syntax"):
            key, _, value = stripped.partition(" ")
            if key != "@syntax" or not value:
                raise ValueError(f"line {lineno}: @syntax requires text")
            if current.syntax:
                raise ValueError(f"line {lineno}: duplicate @syntax for {current.tag}")
            current.syntax = value.strip()
            continue
        if not body_started and stripped.startswith("@"):
            raise ValueError(f"line {lineno}: unknown metadata tag {stripped}")
        if not body_started and not stripped:
            continue
        body.append(line.rstrip("\r\n") + "\n")

    finish(len(text.splitlines()) or 1)
    return Document(atoms)
