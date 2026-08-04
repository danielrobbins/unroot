# Docatoms v1

Docatoms is an incubating, renderer-neutral documentation microformat. Unroot
is its second consumer after Keychain and deliberately begins with Keychain's
existing feature set. This document describes the implemented baseline, not a
promise that the syntax is final.

The build-time pipeline is:

```text
authored atoms -> validated semantic outline -> JSON / Markdown / man(7)
```

The generated JSON is the canonical compiled representation. Renderers consume
the outline recorded there rather than deriving their own document order.
Runtime programs may package and render the JSON without carrying the parser or
compiler in their shipped artifacts.

## Atom records

Every record begins with a tagged heading:

```text
== @KIND NAME: SHORT HELP [parent=KIND:NAME, priority=50, title=DISPLAY TITLE]

@syntax command [OPTIONS]

Body text begins here.
```

`@syntax` is optional and must appear before body text. Metadata is optional.
Metadata values are currently unquoted strings separated by commas.

Implemented metadata:

- `parent` establishes the semantic hierarchy by stable atom tag.
- `priority` is an integer used for coarse ordering; the default is 50.
- `title` overrides the display heading without changing the stable atom tag.

Siblings sort by priority, then display title, then stable tag. Physical source
order does not control rendered order. Exactly one `@tool` atom is required,
parent references must resolve, and parent cycles are rejected.

Kinds are open-ended. The current Unroot source uses `tool`, `topic`, `section`,
`option`, `feature`, and `file`. A `section` may have an empty body when its
children provide the content.

## Body markup

The v1 baseline carries over the minimal markup already used by Keychain:

- paragraphs separated by blank lines;
- inline code in single backticks;
- single-asterisk emphasis;
- bullet lists beginning with `* `;
- numbered lists beginning with `N. `;
- four-space-indented code blocks; and
- triple-backtick fenced code blocks, optionally with a language label.

Markdown output preserves this markup directly. The man renderer maps inline
code to a bold font and single-asterisk emphasis to italics. Terminal consumers
may choose a different visual treatment while preserving the same semantic
role.

## Deliberately deferred markup

The baseline does not yet define:

- a distinction between `*italic*` and `**strong**`;
- links and cross-references;
- nested lists or definition lists in authored body text;
- block quotations, tables, images, or body-level headings;
- delimiter escaping; or
- raw Markdown, HTML, or roff passthrough.

These gaps should be addressed from shared requirements observed in both
Keychain and Unroot. Renderer-specific syntax must not be added to solve a
single output problem.

## Generated files

For Unroot, `scripts/build_docs.py` compiles `docs/unroot.docatoms` into:

- `docs/unroot.docs.json`, the versioned atom catalog and semantic outline;
- `docs/unroot.md`, portable Markdown for GitHub and web publication; and
- `docs/unroot.1`, direct `man(7)` roff source.

All three files are deterministic and checked into the repository. Run
`make docs` to regenerate them and `make docs-check` to reject stale output.
POD and the Perl `pod2man`/`pod2html` toolchain are not part of this pipeline.
