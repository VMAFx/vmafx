#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Enforce optional BuildKit-secret transport for the NEO GitHub token."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
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


def validate_callers(workflow: str, docs: str, fetcher: str) -> list[str]:
    errors: list[str] = []
    build_option = "--secret id=github_token,env=GITHUB_TOKEN"
    if build_option not in workflow or "GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}" not in workflow:
        errors.append("the raw CI build must pass GITHUB_TOKEN as github_token")
    if "--build-arg GITHUB_TOKEN" in workflow + docs + fetcher:
        errors.append("token-valued Docker build arguments are forbidden")
    if build_option not in docs:
        errors.append("dev-mcp docs must show the authenticated raw-build secret")
    if "env -u GITHUB_TOKEN docker build" not in docs:
        errors.append("dev-mcp docs must retain an anonymous raw-build command")
    if build_option not in fetcher:
        errors.append("the NEO rate-limit remedy must name the BuildKit secret")
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
