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

Exit 0 when every file passes, 1 on a finding.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]  # PyYAML ships no stubs in the hook env, as in scripts/ci/check_composite_actions.py

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
STEP_START = re.compile(r"^(\s*)- ")
MAX_FILES = 512
MAX_STEPS = 1024


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


def check_file(path: Path) -> list[str]:
    """Findings for one workflow or composite action."""
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        return []
    if isinstance(doc.get("runs"), dict):
        return check_steps(f"{path}", list(doc["runs"].get("steps") or []))
    findings: list[str] = []
    for name, job in (doc.get("jobs") or {}).items():
        if isinstance(job, dict):
            findings.extend(check_steps(f"{path} job {name}", list(job.get("steps") or [])))
    return findings


def _step_end(lines: list[str], start: int, indent: int) -> int:
    end = start + 1
    while end < len(lines):
        line = lines[end]
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        end += 1
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    return end


def _twin_lines(block: list[str], kind: str, inputs: set[str], cond: str | None) -> list[str]:
    head = block[0]
    indent = len(head) - len(head.lstrip())
    pad = " " * (indent + 2)
    step_keys = [ln for ln in block if len(ln) - len(ln.lstrip(" -")) == indent + 2]
    name = next(
        (
            ln.split("name:", 1)[1].strip()
            for ln in step_keys
            if ln.lstrip(" -").startswith("name:")
        ),
        "",
    )
    name = (name.strip("'\"") or f"{kind.capitalize()} artifact") + " (Gitea)"
    out = [
        " " * indent
        + "- name: "
        + yaml.safe_dump(name, width=1000).strip().removesuffix("...").strip()
    ]
    out.append(pad + "if: " + guarded(cond, True))
    if inputs <= V3_INPUTS[kind]:
        out.append(pad + "uses: " + V3_PINS[kind])
        keep = False
        for line in block:
            stripped = line.strip().lstrip("- ")
            if stripped.startswith("with:"):
                keep = True
            elif keep and line.strip() and len(line) - len(line.lstrip()) <= indent + 2:
                keep = False
            if keep:
                out.append(line)
        return out
    missing = ", ".join(sorted(inputs - V3_INPUTS[kind]))
    reason = f"::warning::artifact {kind} {SKIP_MARKER}: v4+ is GitHub-only, v3 lacks {missing}"
    out.append(pad + "shell: bash")
    out.append(pad + "run: |")
    out.append(pad + "  echo '" + reason + "'")
    return out


def _guard_block(block: list[str], cond: str | None) -> list[str]:
    head = block[0]
    indent = len(head) - len(head.lstrip())
    new_if = " " * (indent + 2) + "if: " + guarded(cond, False)
    for i, line in enumerate(block):
        if re.match(r"^\s*(- )?if:", line):
            if line.lstrip().startswith("- "):
                block[i] = " " * indent + "- if: " + guarded(cond, False)
            else:
                block[i] = new_if
            return block
    return (
        [block[0], new_if, *block[1:]]
        if not head.strip().startswith("- uses:")
        else [
            " " * indent + "- if: " + guarded(cond, False),
            " " * (indent + 2) + head.strip()[2:],
            *block[1:],
        ]
    )


def write_file(path: Path) -> bool:
    """Add the guard and the Gitea twin to every v4+ step in ``path`` that lacks them."""
    if not check_file(path):
        return False
    lines = path.read_text(encoding="utf-8").split("\n")
    out: list[str] = []
    i = 0
    changed = False
    while i < len(lines):
        start = STEP_START.match(lines[i])
        if start is None:
            out.append(lines[i])
            i += 1
            continue
        end = _step_end(lines, i, len(start.group(1)))
        block = lines[i:end]
        parsed = yaml.safe_load("\n".join(ln[len(start.group(1)) :] for ln in block))
        step = (
            parsed[0] if isinstance(parsed, list) and parsed and isinstance(parsed[0], dict) else {}
        )
        cls = _classify(step.get("uses"))
        if cls is None or cls[1] or _base_condition(step.get("if"), on_gitea=False) is not None:
            out.extend(block)
            i = end
            continue
        cond = step.get("if")
        cond_text = None if cond is None else str(cond)
        inputs = set((step.get("with") or {}).keys())
        out.extend(_guard_block(list(block), cond_text))
        out.extend(_twin_lines(block, cls[0], inputs, cond_text))
        changed = True
        i = end
    if changed:
        path.write_text("\n".join(out), encoding="utf-8")
    return changed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check that every artifact step has a Gitea path.")
    parser.add_argument("--write", action="store_true", help="add missing guards and twins")
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
