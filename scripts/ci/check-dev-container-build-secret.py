#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Enforce optional BuildKit-secret transport for the NEO GitHub token.

The token reaches exactly one RUN (the Intel NEO release lookup) as the
optional BuildKit secret ``github_token``. Which dev/Containerfile stages
consume it is derived from the stage graph, not hard-coded: a stage consumes
the secret when it, a stage it is built FROM, or a stage it copies from
mounts it, transitively. The shared stage
builder (ADR-0819, ADR-1346) must forward the secret for exactly those
targets, and every workflow step that calls the builder must hand it
GITHUB_TOKEN exactly when its target consumes the secret.
"""

from __future__ import annotations

import importlib.util
import re
import shlex
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE_BUILDER = "scripts/ci/build-dev-container-stage.sh"
BUILD_OPTION = "--secret id=github_token,env=GITHUB_TOKEN"
CALLER_ENV_LINE = "GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}"
TOKEN_DECLARATION = re.compile(r"(?m)^\s*(?:ARG|ENV)\s+GITHUB_TOKEN(?:\s|=)")
MOUNT = re.compile(r"--mount=(?P<options>[^\s\\]+)")
# Every workflow that builds a stage image through STAGE_BUILDER, with the
# targets it must build. dev-container-build.yml builds libvmaf-build for the
# PR gate and build-deps for the release rehearsal; supply-chain.yml builds
# build-deps for the release itself.
STAGE_CALLERS = {
    ".github/workflows/dev-container-build.yml": ("raw CI build", ("libvmaf-build", "build-deps")),
    ".github/workflows/supply-chain.yml": ("release build", ("build-deps",)),
}
INVOCATION = re.compile(
    r"bash " + re.escape(STAGE_BUILDER) + r"[ \t]+(?:\\\n[ \t]*)?(?P<target>[^\s\\]+)"
)
SCRIPT_ARM = re.compile(r"(?ms)^[ \t]*(?P<target>[a-z][a-z0-9-]*)\)[ \t]*\n(?P<body>.*?)^[ \t]*;;")
STEP_START = re.compile(r"^(?P<indent>[ \t]*)- [A-Za-z][A-Za-z-]*:")
TOKEN_KEY = re.compile(r"^(?P<indent>[ \t]*)GITHUB_TOKEN[ \t]*:")
# "FROM image AS name" is three tokens.
FROM_AS_TOKENS = 3
# A workflow-level `env:` key sits at indent 2 and a job-level one at 6; step
# keys are deeper. Either of the first two would reach every step of the job.
MAX_SHARED_ENV_INDENT = 6

_SPEC = importlib.util.spec_from_file_location(
    "container_image_references", ROOT / "scripts/ci/check-container-image-references.py"
)
assert _SPEC is not None and _SPEC.loader is not None
_PARSER = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(_PARSER)


def _mount_options(containerfile: str) -> list[dict[str, str]]:
    mounts: list[dict[str, str]] = []
    for match in MOUNT.finditer(containerfile):
        options: dict[str, str] = {}
        for field in match.group("options").split(","):
            key, separator, value = field.partition("=")
            options[key] = value if separator else ""
        if options.get("type") == "secret" and options.get("id") == "github_token":
            mounts.append(options)
    return mounts


def secret_stages(containerfile: str) -> tuple[set[str], set[str]]:
    """Return (every named stage, the stages whose build reaches the secret mount).

    Building a stage builds its FROM parent and every stage it copies from, so a
    stage consumes the secret when any stage in that dependency closure mounts it.
    """
    deps: dict[str, set[str]] = {}
    mounting: set[str] = set()
    current = ""
    for _, command, arguments in _PARSER.logical_instructions(containerfile):
        if command == "FROM":
            tokens = [token for token in shlex.split(arguments) if not token.startswith("--")]
            named = len(tokens) >= FROM_AS_TOKENS and tokens[1].upper() == "AS"
            current = tokens[2].lower() if named else ""
            if current:
                deps[current] = {tokens[0].lower()}
        elif not current:
            continue
        elif command == "COPY":
            deps[current].update(
                token.split("=", 1)[1].lower()
                for token in shlex.split(arguments)
                if token.startswith("--from=")
            )
        elif command == "RUN" and _mount_options(arguments):
            mounting.add(current)
    consumers: set[str] = set()
    for stage in deps:
        pending, seen = [stage], set()
        while pending:
            node = pending.pop()
            if node in seen or node not in deps:
                continue
            seen.add(node)
            pending.extend(deps[node])
        if seen & mounting:
            consumers.add(stage)
    return set(deps), consumers


def validate_containerfile(text: str) -> list[str]:
    errors: list[str] = []
    if TOKEN_DECLARATION.search(text):
        errors.append("dev/Containerfile must not declare GITHUB_TOKEN with ARG or ENV")
    mounts = _mount_options(text)
    if len(mounts) != 1:
        errors.append("dev/Containerfile must have exactly one github_token secret mount")
    elif mounts[0].get("env") != "GITHUB_TOKEN" or mounts[0].get("required") != "false":
        errors.append("github_token must mount as optional GITHUB_TOKEN (required=false)")
    mount_at = text.find("--mount=type=secret,id=github_token")
    block_end = text.find("\n\n", mount_at)
    block = text[mount_at:block_end] if mount_at >= 0 and block_end >= 0 else ""
    if "fetch-intel-neo.py" not in block:
        errors.append("github_token must be scoped to the NEO fetch RUN")
    if 'token_args=(--github-token "${GITHUB_TOKEN}")' not in block:
        errors.append("the NEO fetch RUN must ignore an empty github_token secret")
    return errors


def validate_compose(text: str) -> list[str]:
    errors: list[str] = []
    source = re.compile(
        r"(?m)^secrets:\n(?:  #.*\n)*  github_token:\n    environment: GITHUB_TOKEN$"
    )
    grant = "      secrets:\n        - source: github_token\n          target: github_token"
    if not source.search(text):
        errors.append("Compose must source github_token from the host GITHUB_TOKEN")
    if grant not in text:
        errors.append("Compose must grant github_token only to the dev-mcp build")
    if re.search(r"(?m)^\s{4,}GITHUB_TOKEN\s*:", text):
        errors.append("Compose must not expose GITHUB_TOKEN to a runtime service")
    return errors


def _code(script: str) -> str:
    return "\n".join(line for line in script.splitlines() if not line.lstrip().startswith("#"))


def _joined_commands(text: str) -> list[str]:
    """Shell or YAML text with backslash continuations joined, comments dropped."""
    return [
        line.strip()
        for line in re.sub(r"\\\n[ \t]*", " ", text).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def _validate_builder_arms(code: str, containerfile: str) -> list[str]:
    """Each allowlisted target forwards the secret exactly when its stage consumes it."""
    errors: list[str] = []
    stages, consumers = secret_stages(containerfile)
    arms = {
        match.group("target"): match.group("body").replace('"', "").replace("'", "")
        for match in SCRIPT_ARM.finditer(code)
    }
    if not arms:
        errors.append("the stage build script must allowlist its targets in a case statement")
    for target, body in sorted(arms.items()):
        if target not in stages:
            errors.append(f"the stage build script allows {target}, not a dev/Containerfile stage")
        elif target in consumers and BUILD_OPTION not in body:
            errors.append(
                f"the stage build script must pass GITHUB_TOKEN as github_token for {target}"
            )
        elif target not in consumers and "--secret" in body:
            errors.append(f"the stage build script must not pass a secret for {target}")
    return errors


def _validate_builder_command(code: str) -> list[str]:
    """One docker build, fed the target and the arm's secret, and nothing shared."""
    errors: list[str] = []
    builds = [line for line in _joined_commands(code) if line.startswith("docker build")]
    if len(builds) != 1:
        errors.append("the stage build script must run exactly one docker build")
    for build in builds:
        if '--target "$target"' not in build or '"${secret_args[@]}"' not in build:
            errors.append("the docker build must take --target and the secret from the case arm")
        if "--secret" in build:
            errors.append("the docker build must not pass a secret for every target")
    if "--build-arg" in code:
        errors.append("the stage build script must not pass Docker build arguments")
    if re.search(r"--cache-(?:from|to)\b", code):
        errors.append("the stage build script must stay cache-free (ADR-1346)")
    return errors


def validate_stage_builder(script: str, containerfile: str) -> list[str]:
    """The one raw `docker build` of a dev/Containerfile stage (ADR-0819, ADR-1346)."""
    code = _code(script)
    return _validate_builder_arms(code, containerfile) + _validate_builder_command(code)


def _step_blocks(workflow: str) -> list[str]:
    """Split a workflow into list-item blocks: each runs to its next sibling or dedent."""
    lines = workflow.splitlines()
    blocks: list[str] = []
    for index, line in enumerate(lines):
        start = STEP_START.match(line)
        if not start:
            continue
        indent = len(start.group("indent"))
        end = index + 1
        while end < len(lines):
            following = lines[end]
            depth = len(following) - len(following.lstrip())
            if following.strip() and depth <= indent:
                break
            end += 1
        blocks.append("\n".join(lines[index:end]))
    return blocks


def _validate_caller_steps(
    workflow: str, label: str, expected: tuple[str, ...], consumers: set[str]
) -> list[str]:
    """Each builder step names an expected target and gets GITHUB_TOKEN iff it is consumed."""
    errors: list[str] = []
    built: list[str] = []
    for block in _step_blocks(workflow):
        match = INVOCATION.search(block)
        if not match:
            continue
        target = match.group("target").strip("\"'")
        built.append(target)
        has_token = any(TOKEN_KEY.match(line) for line in block.splitlines())
        if target in consumers and CALLER_ENV_LINE not in block:
            errors.append(f"the {label} of {target} must set GITHUB_TOKEN from the secret")
        elif target not in consumers and has_token:
            errors.append(f"the {label} of {target} must not receive GITHUB_TOKEN")
    if sorted(built) != sorted(expected):
        errors.append(
            f"the {label} must build {', '.join(expected)} via {STAGE_BUILDER} "
            f"(found: {', '.join(built) or 'none'})"
        )
    return errors


def validate_stage_caller(
    workflow: str, label: str, expected: tuple[str, ...], containerfile: str
) -> list[str]:
    """Each stage build goes through the script, with GITHUB_TOKEN iff it is consumed."""
    _, consumers = secret_stages(containerfile)
    errors = _validate_caller_steps(workflow, label, expected, consumers)
    for line in workflow.splitlines():
        key = TOKEN_KEY.match(line)
        if key and len(key.group("indent")) <= MAX_SHARED_ENV_INDENT:
            errors.append(f"the {label} workflow must not set GITHUB_TOKEN for a whole job")
    for command in _joined_commands(workflow):
        if "docker build" in command and "--check" not in command:
            errors.append(f"the {label} workflow must build stage images only via {STAGE_BUILDER}")
    return errors


def validate_callers(workflow: str, docs: str, fetcher: str, containerfile: str) -> list[str]:
    label, expected = STAGE_CALLERS[".github/workflows/dev-container-build.yml"]
    errors = validate_stage_caller(workflow, label, expected, containerfile)
    if "--build-arg GITHUB_TOKEN" in workflow + docs + fetcher:
        errors.append("token-valued Docker build arguments are forbidden")
    if BUILD_OPTION not in docs:
        errors.append("dev-mcp docs must show the authenticated raw-build secret")
    if "env -u GITHUB_TOKEN docker build" not in docs:
        errors.append("dev-mcp docs must retain an anonymous raw-build command")
    if BUILD_OPTION not in fetcher:
        errors.append("the NEO rate-limit remedy must name the BuildKit secret")
    return errors


def validate_publish(workflow: str) -> list[str]:
    """The publish job builds the same NEO step, so it needs the same secret."""
    errors: list[str] = []
    if not re.search(
        r"(?m)^\s+secrets: \|\n\s+github_token=\$\{\{ secrets\.GITHUB_TOKEN \}\}$", workflow
    ):
        errors.append("the dev-container publish build must pass github_token as a BuildKit secret")
    if re.search(r"(?m)^\s+build-args:[^\n]*\n(?:\s+[^\n]*\n)*?\s+GITHUB_TOKEN=", workflow):
        errors.append("token-valued Docker build arguments are forbidden")
    return errors


RELEASE_SCRIPT = "scripts/release/build-native-release-artifacts.sh"
RELEASE_IMAGE = '"vmafx-release-build:${GITHUB_SHA}"'
RELEASE_RUN_FLAGS = ("--pull never", "--network none")


def _release_run(workflow: str, label: str) -> tuple[str | None, list[str]]:
    """The one `docker run` that compiles the release, without its version argument."""
    runs = [
        command
        for command in _joined_commands(workflow)
        if command.startswith("docker run ") and RELEASE_SCRIPT in command
    ]
    if len(runs) != 1:
        return None, [
            f"{label} must run {RELEASE_SCRIPT} in exactly one docker run (found {len(runs)})"
        ]
    command = runs[0]
    errors = [
        f"{label} release docker run lacks {flag}"
        for flag in RELEASE_RUN_FLAGS
        if flag not in command.split(" ", 1)[1]
    ]
    if f" {RELEASE_IMAGE} " not in f" {command} ":
        errors.append(
            f"{label} release docker run must use the image built in the same job, {RELEASE_IMAGE}"
        )
    # The last word is the version, which legitimately differs between callers.
    return command.rsplit(" ", 1)[0], errors


def validate_release_rehearsal(release: str, gate: str) -> list[str]:
    """ADR-1346: the PR gate rehearses exactly the release job's container run."""
    release_run, errors = _release_run(release, "supply-chain.yml build-artifacts")
    gate_run, gate_errors = _release_run(gate, "the Dev Container gate rehearsal")
    errors.extend(gate_errors)
    if release_run is not None and gate_run is not None and release_run != gate_run:
        errors.append(
            "the Dev Container gate rehearsal must run the same docker run as build-artifacts"
        )
    return errors


def validate_tree(root: Path = ROOT) -> list[str]:
    def read(path: str) -> str:
        return (root / path).read_text(encoding="utf-8")

    containerfile = read("dev/Containerfile")
    errors = validate_containerfile(containerfile)
    errors.extend(validate_compose(read("dev/docker-compose.yml")))
    errors.extend(
        validate_callers(
            read(".github/workflows/dev-container-build.yml"),
            read("docs/development/dev-mcp.md"),
            read("dev/scripts/fetch-intel-neo.py"),
            containerfile,
        )
    )
    errors.extend(validate_publish(read(".github/workflows/dev-container-publish.yml")))
    errors.extend(validate_stage_builder(read(STAGE_BUILDER), containerfile))
    label, expected = STAGE_CALLERS[".github/workflows/supply-chain.yml"]
    errors.extend(
        validate_stage_caller(
            read(".github/workflows/supply-chain.yml"), label, expected, containerfile
        )
    )
    errors.extend(
        validate_release_rehearsal(
            read(".github/workflows/supply-chain.yml"),
            read(".github/workflows/dev-container-build.yml"),
        )
    )
    return errors


def main() -> int:
    errors = validate_tree()
    if errors:
        for error in errors:
            print(f"dev-container-build-secret: {error}", file=sys.stderr)
        return 1
    print("dev-container-build-secret: OK — optional BuildKit secret, anonymous fallback retained")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
