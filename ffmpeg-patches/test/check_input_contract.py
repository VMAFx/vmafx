#!/usr/bin/env python3
"""Fail closed when documented FFmpeg VMAF filter pads are reversed."""

# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

from __future__ import annotations

import argparse
import re
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path

WRONG_EXAMPLE_MARKER = "<!-- vmafx-ffmpeg-input-order: intentionally-wrong -->"
PATCH_NAME = "0001-libvmaf-add-tiny-model-option.patch"
FILTER_NAME = r"libvmaf(?:_[a-z0-9_]+)?"
FILTER_RE = re.compile(rf"(?<![a-zA-Z0-9_]){FILTER_NAME}(?=[\s=,;'\"\[]|$)")
PADDED_FILTER_RE = re.compile(rf"\[\s*([^\]]+?)\s*\]\s*\[\s*([^\]]+?)\s*\]\s*({FILTER_NAME})\b")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})([^`]*)$")
LOG_STATEMENT_RE = re.compile(
    r"av_log\(ctx,\s*AV_LOG_INFO,\s*"
    r'"libvmaf: input\[0\]=distorted input\[1\]=reference "\s*'
    r'"\(opposite of Python runner / vmaf CLI order\)\\n"\s*\);',
    re.DOTALL,
)

REFERENCE_WORDS = {
    "clean",
    "gt",
    "original",
    "r",
    "ref",
    "reference",
    "source",
    "src",
}
DISTORTED_WORDS = {
    "d",
    "degraded",
    "dis",
    "dist",
    "distorted",
    "encoded",
    "input",
    "main",
    "out",
    "output",
}
REQUIRED_INPUT_COUNT = 2
MIN_GRAPH_LABELS = 2
REQUIRED_WORKFLOW_JOBS = ("check", "refresh")
DOCUMENTED_IMAGE_BASENAMES = {"vmaf", "ffmpeg_vmaf"}
CONTAINER_RUNTIMES = {"docker", "docker.exe", "podman", "podman.exe"}
SUPPORTED_CONTAINER_FLAGS = {"--rm"}
SUPPORTED_CONTAINER_VALUE_OPTIONS = {
    "-v",
    "--volume",
    "--gpus",
    "-e",
    "--env",
    "--entrypoint",
    "--name",
}


@dataclass(frozen=True)
class FencedBlock:
    path: Path
    start_line: int
    text: str
    intentionally_wrong: bool


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    message: str

    def render(self, root: Path) -> str:
        try:
            display = self.path.relative_to(root)
        except ValueError:
            display = self.path
        return f"{display}:{self.line}: {self.message}"


def role_from_name(value: str) -> str | None:
    """Infer only explicit human role names; broad substring guesses are unsafe."""

    lowered = Path(value.strip("'\"")).name.lower()
    if "src01_hrc00" in lowered or "seeking_30_480_1050" in lowered:
        return "reference"
    if "src01_hrc01" in lowered or "seeking_10_288_375" in lowered:
        return "distorted"

    words = set(filter(None, re.split(r"[^a-z0-9]+", lowered)))
    reference = bool(words & REFERENCE_WORDS)
    distorted = bool(words & DISTORTED_WORDS)
    if reference == distorted:
        return None
    return "reference" if reference else "distorted"


def markdown_files(root: Path) -> list[Path]:
    docs = root / "docs"
    result: list[Path] = []
    if docs.is_dir():
        for path in docs.rglob("*.md"):
            relative = path.relative_to(docs)
            if relative.parts[0] in {"adr", "research"}:
                continue
            if relative.as_posix() in {"rebase-notes.md", "state.md"}:
                continue
            result.append(path)
    readme = root / "ffmpeg-patches/README.md"
    if readme.is_file():
        result.append(readme)
    return sorted(result)


def fenced_blocks(path: Path) -> tuple[list[FencedBlock], list[Finding], int]:
    lines = path.read_text(encoding="utf-8").splitlines()
    blocks: list[FencedBlock] = []
    findings: list[Finding] = []
    marker_count = sum(line.strip() == WRONG_EXAMPLE_MARKER for line in lines)
    index = 0
    while index < len(lines):
        match = FENCE_RE.match(lines[index])
        if not match:
            index += 1
            continue
        fence = match.group(1)
        start = index
        index += 1
        body: list[str] = []
        while index < len(lines) and not re.match(
            rf"^\s*{re.escape(fence[0])}{{{len(fence)},}}\s*$", lines[index]
        ):
            body.append(lines[index])
            index += 1
        if index == len(lines):
            findings.append(Finding(path, start + 1, "unterminated Markdown code fence"))
            break
        marked = start > 0 and lines[start - 1].strip() == WRONG_EXAMPLE_MARKER
        blocks.append(FencedBlock(path, start + 1, "\n".join(body), marked))
        index += 1

    recognized = sum(block.intentionally_wrong for block in blocks)
    if recognized != marker_count:
        findings.append(
            Finding(
                path,
                1,
                "intentional-wrong marker must appear immediately before exactly one code fence",
            )
        )
    return blocks, findings, marker_count


def updated_quote(char: str, quote: str | None) -> str | None:
    if quote is None:
        return char
    if quote == char:
        return None
    return quote


def logical_shell_commands(text: str) -> list[str]:
    """Split on unquoted, non-continued newlines while preserving filter graphs."""

    commands: list[str] = []
    current: list[str] = []
    quote: str | None = None
    escaped = False
    index = 0
    while index < len(text):
        char = text[index]
        if escaped:
            if char == "\n":
                current.append(" ")
            else:
                current.extend(("\\", char))
            escaped = False
            index += 1
            continue
        if char == "\\" and quote != "'":
            escaped = True
            index += 1
            continue
        if char in {"'", '"'}:
            quote = updated_quote(char, quote)
            current.append(char)
            index += 1
            continue
        if char == "\n":
            if quote is None:
                command = "".join(current).strip()
                if command:
                    commands.append(command)
                current = []
            else:
                current.append(" ")
            index += 1
            continue
        current.append(char)
        index += 1

    if escaped:
        current.append("\\")
    command = "".join(current).strip()
    if command:
        commands.append(command)
    return commands


def image_basename(image: str) -> str:
    no_digest = image.split("@", maxsplit=1)[0]
    final_segment = no_digest.rsplit("/", maxsplit=1)[-1]
    return final_segment.split(":", maxsplit=1)[0]


def _validate_container_command(
    image_token: str, entrypoint: str | None, forwarded_argv: list[str]
) -> tuple[list[str] | None, str | None]:
    basename = image_basename(image_token)
    is_documented = basename in DOCUMENTED_IMAGE_BASENAMES
    has_ffmpeg_entrypoint = entrypoint is not None and Path(entrypoint).name in {
        "ffmpeg",
        "ffmpeg.exe",
    }

    if is_documented:
        if entrypoint is not None and not has_ffmpeg_entrypoint:
            return None, f"unsupported container entrypoint '{entrypoint}'; expected ffmpeg"
    elif not has_ffmpeg_entrypoint:
        return None, None

    if not forwarded_argv:
        return None, "container command has no forwarded arguments"
    return forwarded_argv, None


def _parse_container_option(tokens: list[str], index: int) -> tuple[int, str | None, str | None]:
    token = tokens[index]
    if token in SUPPORTED_CONTAINER_FLAGS:
        return index + 1, None, None

    name, has_equal, value = token.partition("=")
    if name not in SUPPORTED_CONTAINER_VALUE_OPTIONS:
        return index, None, f"unrecognized or malformed container option '{token}'"

    if has_equal:
        if not value:
            return index, None, f"container option '{name}' requires an argument"
        opt_val = value
        next_idx = index + 1
    else:
        if index + 1 >= len(tokens) or tokens[index + 1].startswith("-"):
            return index, None, f"container option '{name}' requires an argument"
        opt_val = tokens[index + 1]
        next_idx = index + 2

    entrypoint = opt_val if name == "--entrypoint" else None
    return next_idx, entrypoint, None


def parse_container_run(tokens: list[str]) -> tuple[list[str] | None, str | None]:
    """Parse documented docker/podman run commands."""

    index = 0
    entrypoint: str | None = None
    while index < len(tokens):
        token = tokens[index]
        if not token.startswith("-"):
            return _validate_container_command(token, entrypoint, tokens[index + 1 :])

        next_index, option_entrypoint, error = _parse_container_option(tokens, index)
        if error:
            return None, error
        if option_entrypoint:
            entrypoint = option_entrypoint
        index = next_index

    return None, "container command missing image"


def ffmpeg_arguments(command: str) -> tuple[list[str] | None, str | None]:
    """Return the FFmpeg-facing argv or a parse error for a candidate command."""

    if not FILTER_RE.search(command):
        return None, None
    try:
        tokens = shlex.split(command, comments=True, posix=True)
    except ValueError as error:
        return None, f"cannot parse shell command containing a VMAF filter: {error}"

    for index, argument in enumerate(tokens):
        if Path(argument).name in CONTAINER_RUNTIMES:
            if index + 1 < len(tokens) and tokens[index + 1] == "run":
                container_args, error = parse_container_run(tokens[index + 2 :])
                if error or container_args is not None:
                    return container_args, error
                return None, None

    for index, argument in enumerate(tokens):
        if Path(argument).name in {"ffmpeg", "ffmpeg.exe"}:
            return tokens[index + 1 :], None
    return None, None


def extract_inputs(arguments: list[str]) -> tuple[list[str], str | None]:
    inputs: list[str] = []
    index = 0
    while index < len(arguments):
        if arguments[index] == "-i":
            if index + 1 >= len(arguments):
                return inputs, "-i has no input operand"
            inputs.append(arguments[index + 1])
            index += 2
            continue
        index += 1
    return inputs, None


def graph_definitions(graph: str) -> dict[str, str]:
    definitions: dict[str, str] = {}
    for segment in graph.split(";"):
        if FILTER_RE.search(segment):
            continue
        labels = re.findall(r"\[\s*([^\]]+?)\s*\]", segment)
        if len(labels) >= MIN_GRAPH_LABELS:
            definitions[labels[-1]] = labels[0]
    return definitions


def resolve_role(
    label: str,
    inputs: list[str],
    definitions: dict[str, str],
    seen: frozenset[str] = frozenset(),
) -> tuple[str | None, str | None]:
    normalized = label.strip()
    if normalized in seen:
        return None, f"cyclic filter-label definition at [{normalized}]"

    index_match = re.fullmatch(r"(\d+)(?::[vas](?:\d+)?)?", normalized)
    source_role: str | None = None
    if index_match:
        input_index = int(index_match.group(1))
        if input_index >= len(inputs):
            return None, f"filter pad [{normalized}] references missing input {input_index}"
        return role_from_name(inputs[input_index]), None

    if normalized in definitions:
        source_role, error = resolve_role(
            definitions[normalized], inputs, definitions, seen | {normalized}
        )
        if error:
            return None, error

    label_role = role_from_name(normalized)
    if label_role and source_role and label_role != source_role:
        return None, (f"label [{normalized}] says {label_role} but traces to a {source_role} input")
    return label_role or source_role, None


def analyze_command(command: str) -> tuple[list[tuple[str | None, str | None]], list[str]]:
    arguments, parse_error = ffmpeg_arguments(command)
    if parse_error or arguments is None:
        return [], [parse_error] if parse_error else []
    inputs, input_error = extract_inputs(arguments)
    if input_error:
        return [], [input_error]

    joined = " ".join(arguments)
    padded = list(PADDED_FILTER_RE.finditer(joined))
    filters = list(FILTER_RE.finditer(joined))
    if not filters:
        return [], []
    if not inputs and any(argument in {"-h", "-help", "--help"} for argument in arguments):
        return [], []
    if len(inputs) < REQUIRED_INPUT_COUNT:
        return [], ["two-input VMAF filter command has fewer than two -i inputs"]

    definitions = graph_definitions(joined)
    pairs: list[tuple[str | None, str | None]] = []
    errors: list[str] = []
    if not padded:
        # A bare -lavfi libvmaf expression receives inputs 0 and 1 implicitly.
        padded_labels = [("0:v", "1:v")]
    else:
        padded_labels = [(match.group(1), match.group(2)) for match in padded]
        if len(padded) != len(filters):
            errors.append("could not resolve both input pads for every VMAF filter")

    for first, second in padded_labels:
        first_role, first_error = resolve_role(first, inputs, definitions)
        second_role, second_error = resolve_role(second, inputs, definitions)
        if first_error:
            errors.append(first_error)
        if second_error:
            errors.append(second_error)
        pairs.append((first_role, second_role))
    return pairs, errors


def check_log_statement(root: Path) -> list[Finding]:
    patch = root / "ffmpeg-patches" / PATCH_NAME
    if not patch.is_file():
        return [Finding(patch, 1, "required patch is missing")]
    added_runs: list[str] = []
    current: list[str] = []
    for line in patch.read_text(encoding="utf-8").splitlines():
        if line.startswith("+") and not line.startswith("+++"):
            current.append(line[1:])
            continue
        if current:
            added_runs.append("\n".join(current))
            current = []
    if current:
        added_runs.append("\n".join(current))

    executable_runs = [
        re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", run, flags=re.DOTALL))
        for run in added_runs
    ]
    match_count = sum(len(LOG_STATEMENT_RE.findall(run)) for run in executable_runs)
    if match_count != 1:
        return [
            Finding(
                patch,
                1,
                "expected exactly one executable AV_LOG_INFO input-order reminder",
            )
        ]
    return []


def check_wiring(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    makefile = root / "Makefile"
    make_text = makefile.read_text(encoding="utf-8") if makefile.is_file() else ""
    target = re.search(r"(?ms)^ffmpeg-input-contract:\s*\n(?P<body>(?:\t[^\n]*\n)+)", make_text)
    body = target.group("body") if target else ""
    required_make = (
        r"(?m)^\t@?bash ffmpeg-patches/test/check-input-contract\.sh\s*$",
        r"(?m)^\t@?python3 -m unittest discover -s ffmpeg-patches/test "
        r"-p 'test_input_contract\.py' -v\s*$",
    )
    for command in required_make:
        if not re.search(command, body):
            findings.append(Finding(makefile, 1, "ffmpeg-input-contract target is incomplete"))
    if not re.search(r"(?m)^lint-sh:\s+[^\n]*\bffmpeg-input-contract\b", make_text):
        findings.append(Finding(makefile, 1, "lint-sh must depend on ffmpeg-input-contract"))

    workflow = root / ".github/workflows/ffmpeg-patch-stack.yml"
    workflow_text = workflow.read_text(encoding="utf-8") if workflow.is_file() else ""
    job_headers = list(re.finditer(r"(?m)^  ([a-zA-Z0-9_-]+):\s*(?:#.*)?$", workflow_text))
    job_bodies: dict[str, str] = {}
    for index, header in enumerate(job_headers):
        end = job_headers[index + 1].start() if index + 1 < len(job_headers) else len(workflow_text)
        job_bodies[header.group(1)] = workflow_text[header.end() : end]
    workflow_call = re.compile(r"(?m)^\s+run:\s+make ffmpeg-input-contract\s*$")
    for job_name in REQUIRED_WORKFLOW_JOBS:
        if len(workflow_call.findall(job_bodies.get(job_name, ""))) != 1:
            findings.append(
                Finding(
                    workflow,
                    1,
                    f"FFmpeg workflow job {job_name} must run make ffmpeg-input-contract once",
                )
            )

    precommit = root / ".pre-commit-config.yaml"
    precommit_text = precommit.read_text(encoding="utf-8") if precommit.is_file() else ""
    hook = re.search(
        r"(?ms)^\s*- id: ffmpeg-input-contract\n(?P<body>.*?)(?=^\s*- id:|\Z)",
        precommit_text,
    )
    if not hook or not re.search(
        r"(?m)^\s+entry:\s+make ffmpeg-input-contract\s*$", hook.group("body")
    ):
        findings.append(Finding(precommit, 1, "pre-commit must run make ffmpeg-input-contract"))
    return findings


def check_block(block: FencedBlock) -> tuple[list[Finding], int]:
    command_results: list[tuple[str | None, str | None]] = []
    command_errors: list[str] = []
    for command in logical_shell_commands(block.text):
        pairs, errors = analyze_command(command)
        command_results.extend(pairs)
        command_errors.extend(errors)

    if not command_results and not command_errors:
        if block.intentionally_wrong:
            return [
                Finding(
                    block.path,
                    block.start_line,
                    "intentional-wrong marker does not guard a VMAF score command",
                )
            ], 0
        return [], 0

    if block.intentionally_wrong:
        if command_errors or len(command_results) != 1:
            return [
                Finding(
                    block.path,
                    block.start_line,
                    "intentional-wrong fence must contain one unambiguous score command",
                )
            ], 1
        if command_results[0] != ("reference", "distorted"):
            return [
                Finding(
                    block.path,
                    block.start_line,
                    "intentional-wrong fence must demonstrate reference on pad 0 and distorted on pad 1",
                )
            ], 1
        return [], 1

    findings = [Finding(block.path, block.start_line, error) for error in command_errors]
    for first, second in command_results:
        if first is None or second is None:
            findings.append(
                Finding(
                    block.path,
                    block.start_line,
                    "ambiguous VMAF input roles; name or label distorted/main and reference explicitly",
                )
            )
        elif (first, second) != ("distorted", "reference"):
            findings.append(
                Finding(
                    block.path,
                    block.start_line,
                    f"reversed VMAF pads: pad 0 is {first}, pad 1 is {second}",
                )
            )
    return findings, 0


def check_docs(root: Path) -> list[Finding]:
    findings: list[Finding] = []
    marker_count = 0
    marked_blocks = 0
    for path in markdown_files(root):
        blocks, block_findings, count = fenced_blocks(path)
        findings.extend(block_findings)
        marker_count += count
        for block in blocks:
            block_findings, marked = check_block(block)
            findings.extend(block_findings)
            marked_blocks += marked

    if marker_count != 1 or marked_blocks != 1:
        findings.append(
            Finding(
                root / "docs/usage/ffmpeg.md",
                1,
                "exactly one narrowly marked intentional-wrong example is required",
            )
        )
    return findings


def check(root: Path) -> list[Finding]:
    return check_log_statement(root) + check_wiring(root) + check_docs(root)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    findings = check(root)
    if findings:
        print(f"FAIL: {len(findings)} FFmpeg input-contract violation(s)", file=sys.stderr)
        for finding in findings:
            print(f"  {finding.render(root)}", file=sys.stderr)
        return 1
    print("PASS: FFmpeg VMAF input-order contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
