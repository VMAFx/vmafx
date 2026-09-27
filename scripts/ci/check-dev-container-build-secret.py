#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Enforce optional BuildKit-secret transport for the NEO GitHub token."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STAGE_BUILDER = "scripts/ci/build-dev-container-stage.sh"
BUILD_OPTION = "--secret id=github_token,env=GITHUB_TOKEN"
CALLER_ENV_LINE = "GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}"
TOKEN_DECLARATION = re.compile(r"(?m)^\s*(?:ARG|ENV)\s+GITHUB_TOKEN(?:\s|=)")
MOUNT = re.compile(r"--mount=(?P<options>[^\s\\]+)")


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


def validate_stage_builder(script: str) -> list[str]:
    """The one raw `docker build` of the libvmaf-build stage (ADR-0819, ADR-1346)."""
    errors: list[str] = []
    code = "\n".join(line for line in script.splitlines() if not line.lstrip().startswith("#"))
    if BUILD_OPTION not in code or "--target libvmaf-build" not in code:
        errors.append("the stage build script must pass GITHUB_TOKEN as github_token")
    if "--build-arg" in code:
        errors.append("the stage build script must not pass Docker build arguments")
    if re.search(r"--cache-(?:from|to)\b", code):
        errors.append("the stage build script must stay cache-free (ADR-1346)")
    return errors


def validate_stage_caller(workflow: str, label: str) -> list[str]:
    """A workflow that builds the stage must call the shared script with the token."""
    if STAGE_BUILDER not in workflow or CALLER_ENV_LINE not in workflow:
        return [f"the {label} must build via {STAGE_BUILDER} with GITHUB_TOKEN set"]
    return []


def validate_callers(workflow: str, docs: str, fetcher: str) -> list[str]:
    errors = validate_stage_caller(workflow, "raw CI build")
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


def validate_tree(root: Path = ROOT) -> list[str]:
    def read(path: str) -> str:
        return (root / path).read_text(encoding="utf-8")

    errors = validate_containerfile(read("dev/Containerfile"))
    errors.extend(validate_compose(read("dev/docker-compose.yml")))
    errors.extend(
        validate_callers(
            read(".github/workflows/dev-container-build.yml"),
            read("docs/development/dev-mcp.md"),
            read("dev/scripts/fetch-intel-neo.py"),
        )
    )
    errors.extend(validate_publish(read(".github/workflows/dev-container-publish.yml")))
    errors.extend(validate_stage_builder(read(STAGE_BUILDER)))
    errors.extend(
        validate_stage_caller(read(".github/workflows/supply-chain.yml"), "release build")
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
