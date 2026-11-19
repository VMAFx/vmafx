#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check that every artifact step has a Gitea path.

``actions/upload-artifact`` and ``actions/download-artifact`` from v4 on refuse every
server whose host is not github.com, ``*.ghe.com`` or ``*.localhost``
(``GHESNotSupportedError`` from ``@actions/artifact``). A Gitea runner is such a server, so
on a Gitea mirror every job with an artifact step failed at that step. Gitea serves the v3
artifact protocol, and upstream names v3 as the version for servers that are not GitHub.

Every step that uses the v4+ pin therefore runs only off Gitea and is followed by its Gitea
twin; the act runner Gitea ships sets ``GITEA_ACTIONS=true`` in the job environment:

* the v4+ step's ``if:`` is ``${{ env.GITEA_ACTIONS != 'true' }}``, or
  ``${{ (<condition>) && env.GITEA_ACTIONS != 'true' }}`` when it had a condition;
* the next step has the same condition with ``== 'true'`` and either uses the v3 pin of the
  same action with the same ``with:`` inputs, or, for a download that needs an input v3 does
  not have (``pattern``, ``merge-multiple``, ...), is a ``run:`` step that prints why the
  download is skipped on Gitea. Nothing is skipped without a message.

A v3 pin anywhere else is refused, because github.com no longer serves v3.

``--write`` adds the guard and the twin to every v4+ step that has neither, in place.
The writer uses ruamel.yaml's round-trip mode so comments, key order, folded blocks,
quoting and anchors survive the edit; it sets ``width = 1000`` and
``indent(mapping=2, sequence=4, offset=2)`` so a file that needs no twin is left
byte-identical. A file whose unchanged round trip is not byte-identical (for example
flow mappings with inner spaces) is refused rather than reformatted. ruamel.yaml follows
YAML 1.2 for plain scalars: ``on``, ``yes`` and ``no`` are strings, not booleans, so a
``with:`` value such as ``if-no-files-found: yes`` compares as the same string on the
step and its twin.

Exit 0 when every file passes, 1 on a finding.
"""

from __future__ import annotations

import argparse
import io
import re
import sys
from collections.abc import Iterator, MutableMapping, MutableSequence
from copy import deepcopy
from pathlib import Path
from typing import Any

from ruamel.yaml import YAML
from ruamel.yaml.comments import CommentedMap
from ruamel.yaml.error import StringMark
from ruamel.yaml.scalarstring import LiteralScalarString
from ruamel.yaml.tokens import CommentToken

ROOT = Path(__file__).resolve().parents[2]
V3_PINS = {
    "upload": "actions/upload-artifact@c6a366c94c3e0affe28c06c8df20a878f24da3cf  # v3.2.2",
    "download": "actions/download-artifact@a9bc5e6ef2cb54c177f32aa5726adaa15e7e2d59  # v3.1.0",
}
V3_INPUTS = {
    "upload": {"name", "path", "if-no-files-found", "retention-days"},
    "download": {"name", "path"},
}
SKIP_MARKER = "skipped on Gitea"
OFF_GITEA = "env.GITEA_ACTIONS != 'true'"
ON_GITEA = "env.GITEA_ACTIONS == 'true'"
USES = re.compile(r"actions/(upload|download)-artifact@([0-9a-f]{40})")
MAX_FILES = 512
MAX_STEPS = 1024
CA_POST_VALUE = 2


def find_files(root: Path) -> list[Path]:
    """Every workflow and composite-action manifest below ``root``/.github."""
    gh = root / ".github"
    files = sorted(gh.glob("workflows/*.y*ml")) + sorted(gh.glob("actions/*/action.y*ml"))
    return [p for p in files if p.is_file()][:MAX_FILES]


def _v3_sha(kind: str) -> str:
    return V3_PINS[kind].split("@", 1)[1].split()[0]


def _classify(uses: object) -> tuple[str, bool] | None:
    """(kind, is_v3) for an artifact step, None for any other step."""
    match = USES.search(str(uses or ""))
    if match is None:
        return None
    return match.group(1), match.group(2) == _v3_sha(match.group(1))


def _strip_expr(cond: str) -> str:
    """The bare expression of an ``if:`` value."""
    cond = cond.strip()
    if cond.startswith("${{") and cond.endswith("}}"):
        cond = cond[3:-2].strip()
    return cond


def guarded(cond: str | None, on_gitea: bool) -> str:
    """The canonical ``if:`` of a guarded step."""
    tail = ON_GITEA if on_gitea else OFF_GITEA
    if not cond:
        return "${{ " + tail + " }}"
    return "${{ (" + _strip_expr(cond) + ") && " + tail + " }}"


def _base_condition(cond: object, on_gitea: bool) -> str | None:
    """The original condition of a guarded ``if:``, or None when it is not guarded."""
    text = str(cond or "").strip()
    tail = ON_GITEA if on_gitea else OFF_GITEA
    if text == "${{ " + tail + " }}":
        return ""
    prefix, suffix = "${{ (", ") && " + tail + " }}"
    if text.startswith(prefix) and text.endswith(suffix):
        return text[len(prefix) : -len(suffix)]
    return None


def _check_v3_twin(
    where: str, kind: str, step: dict[str, Any], twin: dict[str, Any], inputs: set[str]
) -> list[str]:
    if twin.get("with") != step.get("with"):
        return [f"{where}: Gitea twin has other with: inputs"]
    if not inputs <= V3_INPUTS[kind]:
        return [f"{where}: v3 has no input {sorted(inputs - V3_INPUTS[kind])}"]
    return []


def _check_twin(where: str, kind: str, step: dict[str, Any], twin: object, base: str) -> list[str]:
    if not isinstance(twin, dict):
        return [f"{where}: no Gitea twin step follows"]
    if _base_condition(twin.get("if"), on_gitea=True) != base:
        return [f"{where}: next step is not its Gitea twin (if: {guarded(base, True)})"]
    inputs = set((step.get("with") or {}).keys())
    twin_kind = _classify(twin.get("uses"))
    if twin_kind == (kind, True):
        return _check_v3_twin(where, kind, step, twin, inputs)
    if "run" in twin and SKIP_MARKER in str(twin["run"]) and not inputs <= V3_INPUTS[kind]:
        return []
    return [f"{where}: Gitea twin is neither the v3 pin nor a named skip"]


def check_steps(label: str, steps: list[Any]) -> list[str]:
    """Findings for one ordered list of steps."""
    findings: list[str] = []
    paired: set[int] = set()
    for idx, step in enumerate(steps[:MAX_STEPS]):
        if not isinstance(step, dict):
            continue
        cls = _classify(step.get("uses"))
        if cls is None:
            continue
        kind, is_v3 = cls
        where = f"{label} step {idx + 1} ({step.get('name', kind + '-artifact')})"
        if is_v3:
            if idx not in paired:
                findings.append(f"{where}: v3 pin outside a Gitea twin")
            continue
        base = _base_condition(step.get("if"), on_gitea=False)
        if base is None:
            findings.append(f"{where}: not guarded with {OFF_GITEA}")
            continue
        twin = steps[idx + 1] if idx + 1 < len(steps) else None
        problems = _check_twin(where, kind, step, twin, base)
        findings.extend(problems)
        if not problems:
            paired.add(idx + 1)
    return findings


def _yaml(round_trip: bool = False, explicit_start: bool = False) -> YAML:
    """A YAML parser; ``round_trip`` keeps comments, order and quoting for writes."""
    yaml = YAML(typ="rt" if round_trip else "safe")
    if round_trip:
        yaml.preserve_quotes = True
        yaml.width = 1000
        yaml.explicit_start = explicit_start
        yaml.indent(mapping=2, sequence=4, offset=2)
    return yaml


def _load_yaml(text: str) -> Any:
    """Parse one YAML document with ruamel.yaml's safe constructor."""
    return _yaml().load(text)


def check_file(path: Path) -> list[str]:
    """Findings for one workflow or composite action."""
    doc = _load_yaml(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        return []
    if isinstance(doc.get("runs"), dict):
        return check_steps(f"{path}", list(doc["runs"].get("steps") or []))
    findings: list[str] = []
    for name, job in (doc.get("jobs") or {}).items():
        if isinstance(job, dict):
            findings.extend(check_steps(f"{path} job {name}", list(job.get("steps") or [])))
    return findings


def _is_mapping(value: object) -> bool:
    return isinstance(value, MutableMapping)


def _is_sequence(value: object) -> bool:
    return isinstance(value, MutableSequence)


def _step_lists(doc: Any) -> Iterator[Any]:
    """Every ordered step list in a workflow or composite action."""
    if not _is_mapping(doc):
        return
    runs = doc.get("runs")
    if _is_mapping(runs) and _is_sequence(runs.get("steps")):
        yield runs["steps"]
    jobs = doc.get("jobs")
    if not _is_mapping(jobs):
        return
    for job in jobs.values():
        if _is_mapping(job) and _is_sequence(job.get("steps")):
            yield job["steps"]


def _set_if(step: Any, value: str) -> None:
    """Set ``if:`` while keeping its original position in the step mapping."""
    if "if" in step:
        step["if"] = value
        return
    position = 1 if "name" in step else 0
    insert = getattr(step, "insert", None)
    if insert is None:
        step["if"] = value
    else:
        insert(position, "if", value)


def _last_mapping(step: Any) -> Any:
    """The mapping whose last value carries the step's trailing blank line."""
    with_map = step.get("with") if _is_mapping(step) else None
    return with_map if _is_mapping(with_map) else step


def _last_key(node: Any) -> Any:
    """The last key of a mapping, whose post token carries trailing text."""
    keys = list(node.keys()) if _is_mapping(node) else []
    return keys[-1] if keys else None


def _split_post_token(token: Any) -> tuple[Any, Any]:
    """Split a post-value token at its first line break.

    The part on the value's own line is an end-of-line comment and stays with
    the source step. The remaining comment or blank block belongs after the
    twin. A token with no second line stays with the source.
    """
    value = getattr(token, "value", None)
    if not isinstance(value, str):
        return token, None
    head, sep, tail = value.partition("\n")
    if not sep or not tail:
        return token, None
    column = getattr(token, "_column", 0)
    start = getattr(token, "start_mark", None)
    end = getattr(token, "end_mark", None)
    return (
        CommentToken(head + sep, start_mark=start, end_mark=end, column=column) if head else None,
        CommentToken(sep + tail, start_mark=start, end_mark=end, column=column),
    )


def _take_trailing_post(step: Any) -> Any:
    """Detach and return the source comment/blank token after a step."""
    node = _last_mapping(step)
    key = _last_key(node)
    item = node.ca.items.get(key) if key is not None else None
    if not item or len(item) <= CA_POST_VALUE:
        return None
    token = item[CA_POST_VALUE]
    if token is None:
        return None
    head, tail = _split_post_token(token)
    if tail is None:
        return None
    item[CA_POST_VALUE] = head
    return tail


def _attach_trailing_pre(step: Any, token: Any) -> None:
    """Move a displaced source comment block before the step after the twin."""
    if token is None:
        return
    value = getattr(token, "value", None)
    if isinstance(value, str):
        # The post token starts with the line break that ended the source step.
        # Drop it, and the indentation the source line carried, so the pre
        # comment is indented from its own start mark at the step's column.
        if value.startswith("\n"):
            value = value[1:]
        if not value.startswith("\n"):
            value = value.lstrip(" ")
        token = CommentToken(
            value,
            start_mark=getattr(token, "start_mark", None),
            end_mark=getattr(token, "end_mark", None),
        )
    comment = list(getattr(step.ca, "comment", None) or [None, None])
    pre = list(comment[1] or [])
    pre.append(token)
    comment[1] = pre
    step.ca.comment = comment


def _comment_token(token: Any, value: str, column: int) -> CommentToken:
    """A comment token whose start mark carries the column the emitter indents from."""
    mark = getattr(token, "start_mark", None)
    if mark is None:
        mark = StringMark("<generated>", 0, 0, column, None, None)
    else:
        mark = StringMark(mark.name, mark.index, mark.line, column, mark.buffer, mark.pointer)
    return CommentToken(value, start_mark=mark)


def _attach_trailing_post(step: Any, token: Any, indent: int = 0) -> None:
    """Attach a displaced source comment/blank block after the twin."""
    if token is None:
        return
    node = _last_mapping(step)
    key = _last_key(node)
    if key is None:
        return
    value = getattr(token, "value", None)
    if (
        isinstance(value, str)
        and value.startswith("\n")
        and isinstance(node[key], LiteralScalarString)
    ):
        # A literal block already ends its line; its post token must not add
        # a second line break before the first following comment or blank.
        trimmed = value[1:]
        if not trimmed:
            return
        token = _comment_token(token, trimmed, indent)
    item = list(node.ca.items.get(key) or [None, None, None, None])
    item[CA_POST_VALUE] = token
    node.ca.items[key] = item


def _twin_step(step: Any, kind: str, base: str) -> Any:
    """Build the Gitea twin as a mapping, not as reconstructed text."""
    name = step.get("name")
    label = f"{name} (Gitea)" if name else f"{kind.capitalize()} artifact (Gitea)"
    twin = CommentedMap()
    twin["name"] = label
    twin["if"] = guarded(base or None, True)
    inputs = set((step.get("with") or {}).keys())
    if inputs <= V3_INPUTS[kind]:
        twin["uses"] = V3_PINS[kind].split("#", 1)[0].strip()
        if "with" in step:
            with_map = CommentedMap()
            for key, value in step["with"].items():
                with_map[key] = deepcopy(value)
            twin["with"] = with_map
        twin.yaml_add_eol_comment(V3_PINS[kind].split("#", 1)[1].strip(), key="uses")
    else:
        missing = ", ".join(sorted(inputs - V3_INPUTS[kind]))
        twin["shell"] = "bash"
        twin["run"] = LiteralScalarString(
            f"echo '::warning::artifact {kind} {SKIP_MARKER}: "
            f"v4+ is GitHub-only, v3 lacks {missing}'\n"
        )
    return twin


def _ensure_guard_and_twin(steps: Any, idx: int, kind: str) -> tuple[bool, bool]:
    """Guard a v4+ step and add its twin when it has neither. (changed, inserted)."""
    step = steps[idx]
    original = step.get("if")
    base = _base_condition(original, on_gitea=False)
    changed = False
    if base is None:
        base = str(original or "").strip()
        _set_if(step, guarded(base or None, False))
        changed = True
    if idx + 1 < len(steps):
        twin = steps[idx + 1]
        if not _check_twin("", kind, step, twin, base):
            return changed, False
    steps.insert(idx + 1, _twin_step(step, kind, base))
    return True, True


def write_file(path: Path) -> bool:
    """Add the guard and the Gitea twin to every v4+ step in ``path`` that lacks them."""
    if not check_file(path):
        return False
    text = path.read_text(encoding="utf-8")
    yaml = _yaml(round_trip=True, explicit_start=text.startswith("---"))
    doc = yaml.load(text)
    baseline = io.StringIO()
    yaml.dump(doc, baseline)
    if baseline.getvalue() != text:
        print(
            f"{path}: round-trip without edits is not byte-identical; refusing to rewrite",
            file=sys.stderr,
        )
        return False
    changed = False
    for steps in _step_lists(doc):
        idx = 0
        while idx < len(steps) and idx < MAX_STEPS:
            step = steps[idx]
            if not _is_mapping(step):
                idx += 1
                continue
            cls = _classify(step.get("uses"))
            if cls is None or cls[1]:
                idx += 1
                continue
            did_change, inserted = _ensure_guard_and_twin(steps, idx, cls[0])
            changed = changed or did_change
            if inserted:
                trailing = _take_trailing_post(step)
                next_idx = idx + 2
                if next_idx < len(steps):
                    _attach_trailing_pre(steps[next_idx], trailing)
                else:
                    indent = getattr(getattr(step, "lc", None), "col", 8) - 2
                    _attach_trailing_post(steps[idx + 1], trailing, max(indent, 0))
            idx += 2 if inserted else 1
    if not changed:
        return False
    stream = io.StringIO()
    yaml.dump(doc, stream)
    path.write_text(stream.getvalue(), encoding="utf-8")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check that every artifact step has a Gitea path.")
    parser.add_argument(
        "--write",
        action="store_true",
        help=(
            "add missing guards and twins; refuses files that cannot round-trip "
            "(for example supply-chain.yml), which need a manual edit"
        ),
    )
    parser.add_argument("paths", nargs="*", type=Path, help="files to check (default: all)")
    args = parser.parse_args(argv)
    paths = args.paths or find_files(ROOT)
    if args.write:
        for path in paths:
            if write_file(path):
                print(f"wrote {path}")
    findings = [f for path in paths for f in check_file(path)]
    for finding in findings:
        print(finding)
    if findings:
        print(f"{len(findings)} artifact step(s) without a Gitea path", file=sys.stderr)
        return 1
    print(f"{len(paths)} file(s): every artifact step has a Gitea path")
    return 0


if __name__ == "__main__":
    sys.exit(main())
