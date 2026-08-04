from __future__ import annotations

import json

import pytest

from tools.docatoms import parse, render_man, render_markdown


SOURCE = """\
== @tool sample: sample - demonstrate Docatoms [title=sample, priority=0]

Sample uses `code` and *emphasis*.

== @topic later: Later [priority=30]

Later prose.

== @section commands: Commands [priority=20]

== @action run: Run a command [parent=section:commands, priority=10]

@syntax sample run [OPTIONS]

Run it:

```console
$ sample run
```

== @option quiet: --quiet [parent=action:run]

Suppress ordinary output.
"""


def test_parser_compiles_stable_semantic_outline():
    document = parse(SOURCE)
    payload = json.loads(document.render_json())

    assert payload["schema"] == "docatoms/v1"
    assert [node["tag"] for node in payload["outline"]] == [
        "tool:sample",
        "section:commands",
        "topic:later",
    ]
    assert payload["outline"][1]["children"] == [
        {
            "tag": "action:run",
            "children": [{"tag": "option:quiet", "children": []}],
        }
    ]
    assert payload["atoms"]["action:run"]["syntax"] == "sample run [OPTIONS]"


def test_markdown_and_man_follow_the_same_outline():
    document = parse(SOURCE)
    markdown = render_markdown(document)
    man = render_man(document, "1.0_beta1")

    assert markdown.index("## Commands") < markdown.index("### Run a command")
    assert markdown.index("### Run a command") < markdown.index("#### --quiet")
    assert markdown.index("#### --quiet") < markdown.index("## Later")
    assert "`.SH`" not in markdown

    assert man.index(".SH COMMANDS") < man.index(".SS Run a command")
    assert man.index(".SS Run a command") < man.index(".TP")
    assert man.index(".TP") < man.index(".SH LATER")
    assert r"\fBcode\fR" in man
    assert r"\fIemphasis\fR" in man


@pytest.mark.parametrize(
    ("source", "message"),
    [
        (
            "== @tool one: one\n\nBody.\n\n== @tool two: two\n\nBody.\n",
            "exactly one @tool",
        ),
        (
            "== @tool one: one\n\nBody.\n\n== @topic child: Child [parent=topic:missing]\n\nBody.\n",
            "unknown parent",
        ),
        (
            "== @tool one: one\n\nBody.\n\n== @topic bad: Bad [priority=soon]\n\nBody.\n",
            "invalid priority",
        ),
        (
            "== @tool one: one [priority=50]\n\nBody.\n\n== @topic first: First [priority=10]\n\nBody.\n",
            "must be the first root entry",
        ),
    ],
)
def test_invalid_sources_fail(source, message):
    with pytest.raises(ValueError, match=message):
        parse(source)


def test_duplicate_tags_and_parent_cycles_fail():
    duplicate = SOURCE + "\n== @topic later: Again\n\nBody.\n"
    with pytest.raises(ValueError, match="duplicate doc tag topic:later"):
        parse(duplicate)

    cycle = """\
== @tool sample: sample [title=sample]

Body.

== @topic one: One [parent=topic:two]

Body.

== @topic two: Two [parent=topic:one]

Body.
"""
    with pytest.raises(ValueError, match="parent cycle"):
        parse(cycle)


def test_unterminated_code_fence_fails_during_man_rendering():
    source = """\
== @tool sample: sample [title=sample]

```
not closed
"""
    with pytest.raises(ValueError, match="unterminated fenced code block"):
        render_man(parse(source), "1.0")
