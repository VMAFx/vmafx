#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Open a pull request on the Homebrew tap that puts a release's formula in Formula/vmafx.rb.

The release job writes the formula with ``distribution.py homebrew`` and runs this script
with a token that may write the tap's contents and pull requests. The script makes one
branch ``vmafx-<version>`` from the tap's default branch, commits the formula there through
the contents API (GitHub signs the commit; the message carries the sign-off the tap's CI
requires) and opens the pull request; the tap's own required checks decide the merge.
It prints the plan and changes nothing unless ``--apply`` is given, and it changes nothing
when the tap already holds the same formula. Guide: docs/development/distribution.md.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import urllib.error
import urllib.request
from collections.abc import Callable
from pathlib import Path
from typing import Any

API = "https://api.github.com"
FORMULA_PATH = "Formula/vmafx.rb"
TAP = re.compile(r"^[A-Za-z0-9-]+/[A-Za-z0-9._-]+$")
VERSION = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$")
SIGNOFF = re.compile(r"^[^<>\n]+ <[^<>\s]+@[^<>\s]+>$")
TIMEOUT_SECONDS = 30
# GitHub answers 422 for a branch or a pull request that already exists.
HTTP_UNPROCESSABLE = 422

# (method, path, body) -> (status, decoded JSON or None)
Transport = Callable[[str, str, dict[str, Any] | None], tuple[int, Any]]


class PublishError(Exception):
    """An argument, an API answer or a tap state this script refuses."""


def github_transport(token: str) -> Transport:
    def call(method: str, path: str, body: dict[str, Any] | None) -> tuple[int, Any]:
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(API + path, data=data, method=method)  # noqa: S310
        request.add_header("Authorization", f"Bearer {token}")
        request.add_header("Accept", "application/vnd.github+json")
        request.add_header("X-GitHub-Api-Version", "2022-11-28")
        try:
            with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as answer:  # noqa: S310
                raw = answer.read()
                return answer.status, json.loads(raw) if raw else None
        except urllib.error.HTTPError as error:
            raw = error.read()
            try:
                return error.code, json.loads(raw) if raw else None
            except json.JSONDecodeError:
                return error.code, None
        except (urllib.error.URLError, TimeoutError) as error:
            raise PublishError(f"{method} {path}: {error}") from error

    return call


def expect(answer: tuple[int, Any], allowed: tuple[int, ...], what: str) -> Any:
    status, body = answer
    if status not in allowed:
        message = body.get("message") if isinstance(body, dict) else body
        raise PublishError(f"{what}: HTTP {status}: {message}")
    return body


def commit_message(version: str, signoff: str) -> str:
    return f"feat(vmafx): vmafx {version}\n\nSigned-off-by: {signoff}\n"


def validate(tap: str, version: str, signoff: str, formula: str) -> None:
    if not TAP.match(tap):
        raise PublishError(f"--tap {tap!r} is not OWNER/REPOSITORY")
    if not VERSION.match(version):
        raise PublishError(f"--version {version!r} is not MAJOR.MINOR.PATCH[-PRE]")
    if not SIGNOFF.match(signoff):
        raise PublishError(f"--signoff {signoff!r} is not 'Name <address>'")
    if 'url "' not in formula or f"v{version}.tar.gz" not in formula:
        raise PublishError(f"the formula does not name the v{version} source archive")


def publish(call: Transport, tap: str, version: str, formula: str, signoff: str,
            apply: bool) -> str:  # fmt: skip
    """Open (or find) the tap pull request; returns what was done, for the job log."""
    validate(tap, version, signoff, formula)
    repo = expect(call("GET", f"/repos/{tap}", None), (200,), "read the tap")
    base = repo["default_branch"]
    ref = expect(call("GET", f"/repos/{tap}/git/ref/heads/{base}", None), (200,), "read base")
    current = expect(call("GET", f"/repos/{tap}/contents/{FORMULA_PATH}?ref={base}", None),
                     (200, 404), "read the formula")  # fmt: skip
    if isinstance(current, dict) and "content" in current:
        if base64.b64decode(current["content"]).decode() == formula:
            return f"unchanged: {tap} {base} already holds the vmafx {version} formula"
    branch = f"vmafx-{version}"
    if not apply:
        return (f"dry run: would branch {branch} from {tap} {base} at {ref['object']['sha']}, "
                f"commit {FORMULA_PATH} and open a pull request; pass --apply")  # fmt: skip
    made = call("POST", f"/repos/{tap}/git/refs",
                {"ref": f"refs/heads/{branch}", "sha": ref["object"]["sha"]})  # fmt: skip
    if made[0] == HTTP_UNPROCESSABLE:
        # The branch is this script's own from an earlier run of the same version.
        expect(call("PATCH", f"/repos/{tap}/git/refs/heads/{branch}",
                    {"sha": ref["object"]["sha"], "force": True}), (200,), "reset branch")  # fmt: skip
    else:
        expect(made, (201,), "create the branch")
    on_branch = expect(call("GET", f"/repos/{tap}/contents/{FORMULA_PATH}?ref={branch}", None),
                       (200, 404), "read the branch formula")  # fmt: skip
    put: dict[str, Any] = {"message": commit_message(version, signoff), "branch": branch,
                           "content": base64.b64encode(formula.encode()).decode()}  # fmt: skip
    if isinstance(on_branch, dict) and "sha" in on_branch:
        put["sha"] = on_branch["sha"]
    expect(call("PUT", f"/repos/{tap}/contents/{FORMULA_PATH}", put), (200, 201), "commit")
    owner = tap.split("/", maxsplit=1)[0]
    opened = call("POST", f"/repos/{tap}/pulls",
                  {"title": f"feat(vmafx): vmafx {version}", "head": branch, "base": base,
                   "body": f"Formula for vmafx {version}, written by the vmafx release job "
                           f"from packaging/manifest.toml."})  # fmt: skip
    if opened[0] == HTTP_UNPROCESSABLE:
        found = expect(call("GET", f"/repos/{tap}/pulls?head={owner}:{branch}&state=open", None),
                       (200,), "find the open pull request")  # fmt: skip
        if not found:
            raise PublishError(f"open the pull request: HTTP 422: {opened[1]}")
        return f"updated: {found[0]['html_url']}"
    return f"opened: {expect(opened, (201,), 'open the pull request')['html_url']}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    parser.add_argument("--tap", required=True, help="OWNER/REPOSITORY of the Homebrew tap")
    parser.add_argument("--version", required=True)
    parser.add_argument("--formula", type=Path, required=True,
                        help="file written by distribution.py homebrew")  # fmt: skip
    parser.add_argument("--signoff", required=True, help="'Name <address>' of the committer")
    parser.add_argument("--apply", action="store_true", help="change the tap (default: plan)")
    args = parser.parse_args(argv)
    token = os.environ.get("GH_TOKEN", "")
    try:
        if not token:
            raise PublishError("GH_TOKEN is not set")
        formula = args.formula.read_text(encoding="utf-8")
        print(publish(github_transport(token), args.tap, args.version, formula,
                      args.signoff, args.apply))  # fmt: skip
    except (OSError, PublishError) as error:
        print(f"publish-homebrew-tap: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
