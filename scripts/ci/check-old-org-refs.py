#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reject obsolete GitHub organisation references in tracked text.

Exit: 0 clean, 1 finding, 2 usage error.
"""

from __future__ import annotations

import argparse
import codecs
import datetime as dt
import fnmatch
import os
import re
import shutil
import sys
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    CommandFailed,
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command

_GIT_PATH = shutil.which("git")
GIT = str(Path(_GIT_PATH).resolve(strict=True)) if _GIT_PATH is not None else None
MAX_GIT_OUTPUT_BYTES = 16 * 1_048_576
MAX_TEXT_BYTES = 8 * 1_048_576
BINARY_SAMPLE_BYTES = 8192


EXPIRY = dt.date(2026, 11, 30)
ENTRY = dt.date(2026, 11, 20)
INLINE_MARKER = "old-org-" "ok"


class ExceptionRule(NamedTuple):
    paths: tuple[str, ...]
    reason: str
    line_pattern: re.Pattern[str] | None = None
    match_pattern: re.Pattern[str] | None = None
    expiry: dt.date | None = EXPIRY
    entry: dt.date = ENTRY
    mel: str = "C"


PERMANENT = (
    "docs/adr/[0-9][0-9][0-9][0-9]-*.md",
    "docs/adr/_index_fragments/[0-9][0-9][0-9][0-9]-*.md",
    "docs/research/**",
    "scripts/ci/research-digest-id-baseline.json",
    "docs/rebase-notes.d/**",
    "docs/rebase-notes.md",
    # These four paths are rendered by make docs-render from permanent fragments (ADR-2197).
    "docs/adr/README.md",
    "docs/adr/titles.md",
    "docs/adr/by-tag/**",
    "docs/research/titles.md",
    "changelog.d/**",
    "CHANGELOG.md",
    "docs/changelog-archive/**",
    "tools/rc1-tester/image/published-rc/artifacts.json",
)
REPO_SUFFIX = r"""(?:[a-z0-9_.-][^\s`"'<>)]*)?"""
GHCR_REF = re.compile(r"\bghcr\.io/vmafx/" + REPO_SUFFIX, re.I)
EXCEPTIONS = (
    ExceptionRule(
        (
            "core/include/libvmaf/pelorus/*.h",
            "core/src/interop/pelorus_*.c",
            "core/test/test_pelorus_interop.c",
            "docs/api/pelorus-interop.md",
        ),
        "vendored mirror; banner changes when Pelorus is re-vendored from vidiko/pelorus",
    ),
    ExceptionRule(
        ("build-config.env",),
        "ffmpeg-patches v0.7.0 is signed under the VMAFx workflow identity; the pin moves with the next series release",
        line_pattern=re.compile(r"^\s*(?:export\s+)?FFMPEG_FIX_SERIES_REPO\s*="),
    ),
    ExceptionRule(
        (
            "lua/standards.lua",
            "tools/markdownlint/package.json",
            "tools/markdownlint/package-lock.json",
        ),
        "rendered by Praetor; follows when Praetor renames them",
    ),
    ExceptionRule(
        (
            ".github/workflows/e2e-k8s.yml",
            ".github/workflows/helm-chart.yml",
            "api/vmafx-platform.toml",
            "deploy/helm/vmafx/AGENTS.md",
            "deploy/helm/vmafx/templates/NOTES.txt",
            "deploy/helm/vmafx/values.yaml",
            "docker/Dockerfile.e2e-driver",
            "docs/development/k8s-deployment.md",
            "docs/k8s/integration-tests.md",
            "scripts/ci/test_e2e_runtime_contract.py",
            "scripts/ci/tests/test_helm_controller_auth.py",
            "scripts/ci/tests/test_helm_controller_workload.py",
            "test/e2e/controller-ha/driver.yaml",
            "test/e2e/kind-cluster.sh",
        ),
        "k8s and chart image references switch in the follow-up PR once vidiko image tags exist",
        match_pattern=GHCR_REF,
    ),
    ExceptionRule(
        ("docs/usage/tester-image.md",),
        "images published before the move carry the old workflow identity (MEL category D)",
        line_pattern=re.compile(
            r"^An image published before the move \(`ghcr\.io/vmafx/vmafx:<VERSION>-tester`\)"
            r" verifies with `VMAFx` as the organisation in the workflow identity\.$"
        ),
        match_pattern=GHCR_REF,
        expiry=dt.date(2027, 3, 20),
        mel="D",
    ),
    ExceptionRule(
        ("docs/development/operator.md", "docs/getting-started/index.md", "docs/server/grpc.md"),
        "examples use specific pre-move release candidate images",
        match_pattern=re.compile(r"ghcr\.io/vmafx/vmafx(?:-[\w-]+)?:v1\.0\.0-rc\.[12]$"),
        expiry=None,
    ),
    ExceptionRule(
        ("docs/development/release.md",),
        "release candidates retain their original package and signing provenance",
        line_pattern=re.compile(
            r"^\s*(?:docker buildx imagetools inspect .*:v1\.0\.0-rc\.1 |"
            r"cosign verify .*@sha256:DIGEST |"
            r'"https://github\.com/|--source-uri .* --source-tag v1\.0\.0-rc\.2$)'
        ),
        match_pattern=re.compile(
            r"(?:ghcr\.io/vmafx/vmafx:v1\.0\.0-rc\.1|"
            r"ghcr\.io/vmafx/vmafx(?:-node)?@sha256:DIGEST|"
            r"https://github\.com/VMAFx[/]vmafx/\.github/workflows/"
            r"docker-publish-(?:production|operator-node)\.yml@refs/heads/master|"
            r"github\.com/VMAFx[/]vmafx)$"
        ),
        expiry=None,
    ),
    ExceptionRule(
        ("docs/licensing.md", "tools/rc1-tester/tests/test_licensing_published_rc.py"),
        "the published release candidate licence record uses the original package names",
        line_pattern=re.compile(r"^\s*(?:- `|(?:- |assert \")The whole `|docker pull )"),
        match_pattern=re.compile(r"ghcr\.io/vmafx/vmafx(?::v1\.0\.0-rc\.[12][\w.-]*|-node)$"),
        expiry=None,
    ),
    ExceptionRule(
        ("tools/rc1-tester/image/licensing.json",),
        "source offers issued for published release candidates retain their original package",
        line_pattern=re.compile(
            r'^\s*"source_offer": .*published after the fact for this already-published image'
        ),
        match_pattern=GHCR_REF,
        expiry=None,
    ),
    ExceptionRule(
        ("docs/state.md",),
        "the dated state ledger records historical image names used in prior runs",
        line_pattern=re.compile(r"^(?:\| \*\*T-|_Updated: )"),
        match_pattern=GHCR_REF,
        expiry=None,
    ),
)
ORG = r"(?:vmafx|cordanallm|golusoris)(?![A-Za-z0-9_-])"
ORG_START = r"(?<![A-Za-z0-9_/-])"
WORKFLOW_PATH = re.compile(r"\.github/workflows/[^/]+\.ya?ml")
WORKFLOW_IMAGE = re.compile(
    r"""(?:^\s*(?:-\s+)?|[{,]\s*)["']?[a-z0-9_.-]*IMAGE[a-z0-9_.-]*["']?\s*:\s*["']?"""
    r"(vmafx/[a-z0-9_./-]+(?:[:@][a-z0-9_.:-]+)?)",
    re.I,
)
URL_ESCAPE = re.compile(r"%2f|%3a", re.I)
URL_DELIMITERS = {"%2f": "/", "%3a": ":"}
PATTERNS = (
    ("h", GHCR_REF),
    ("d", re.compile(r"\b(?:https?://)?github\.com/" + ORG + r"/" + REPO_SUFFIX, re.I)),
    ("e", re.compile(ORG_START + ORG + r"\.github\.io\b", re.I)),
    ("f", re.compile(r"\bgithub\.com/orgs/" + ORG + r"/" + REPO_SUFFIX, re.I)),
    (
        "g",
        re.compile(
            r"""(?:\bowner["']?\s*[:=]\s*["']?|--owner\s+["']?)"""
            r"(?:VMAFx|cordanaLLM|cordanallm|golusoris)(?![A-Za-z0-9_-])",
        ),
    ),
    ("a", re.compile(ORG_START + r"VMAFx/[A-Za-z0-9_.-]+")),
    ("b", re.compile(ORG_START + r"(?:cordanaLLM|cordanallm)/[A-Za-z0-9_.-]+")),
    ("c", re.compile(ORG_START + r"golusoris[/]golusoris\b")),
    ("j", re.compile(r"\brepo:\s*(" + ORG + r"/" + REPO_SUFFIX + r")", re.I)),
)
GO_IDENTITY = re.compile(
    r"github\.com/(?:VMAFx[/]vmafx|golusoris[/]golusoris|cordanaLLM[/]praetor)(?=[/@;=]|$)"
)
GO_PATH_FILES = (
    ".config/lefthook/engine.sh",
    ".github/workflows/standards-gate.yml",
    "Dockerfile.go-server",
    "PRE_MIGRATION_EPIC.md",
    "api/vmafx-platform.toml",
    "buf.gen.yaml",
    "cmd/vmafx-controller/main.go",
    "cmd/vmafx-controller/version_flag_test.go",
    "cmd/vmafx-server/AGENTS.md",
    "docker/Dockerfile.controller",
    "docker/Dockerfile.node",
    "docker/Dockerfile.operator",
    "docs/api/vmafx/bindings.md",
    "docs/api/vmafx/status-codes.md",
    "docs/architecture/grpc-streaming.md",
    "docs/development/inherited-tags.md",
    "docs/development/languages.md",
    "docs/development/praetor-gate.md",
    "docs/development/pre-commit-hooks.md",
    "docs/development/release.md",
    "docs/licensing.md",
    "docs/state.md",
    "docs/usage/go.md",
    "gen/go/AGENTS.md",
    "gen/go/vmafx.pb.go",
    "gen/go/vmafx_api.pb.go",
    "go.mod",
    "go.sum",
    "internal/app/bootstrap/AGENTS.md",
    "pkg/version/version.go",
    "renovate.json",
    "scripts/ci/tests/test_go_libvmaf_linkage_docs.py",
    "scripts/codegen/tests/test_crd_generate.py",
    "scripts/codegen/vmafx_api/emit_proto.py",
    "tools/rc1-tester/image/licensing.json",
    "tools/rc1-tester/tests/test_licensing_go.py",
)
GO_QUOTED_PATH = re.compile(r"""^\s*(?:import\s+)?(?:[a-zA-Z_]\w*\s+|[.]\s+)?["`]$""")


def allowed(path: str) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in PERMANENT)


def exception(path: str, line: str, reference: str) -> ExceptionRule | None:
    for rule in EXCEPTIONS:
        if not any(fnmatch.fnmatchcase(path, pattern) for pattern in rule.paths):
            continue
        if rule.line_pattern and not rule.line_pattern.match(line):
            continue
        if rule.match_pattern and not rule.match_pattern.match(reference):
            continue
        return rule
    return None


def pattern_hits(path: str, line: str) -> list[tuple[str, re.Match[str]]]:
    hits: list[tuple[str, re.Match[str]]] = []
    if WORKFLOW_PATH.fullmatch(path) and not line.lstrip().startswith("#"):
        hits.extend(("i", match) for match in WORKFLOW_IMAGE.finditer(line))
    for letter, pattern in PATTERNS:
        hits.extend((letter, match) for match in pattern.finditer(line))
    return sorted(hits, key=lambda hit: hit[1].start())


def is_go_identity(path: str, prefix: str, reference: str) -> bool:
    if not GO_IDENTITY.match(reference):
        return False
    if prefix.endswith(("pkg:golang/", "mod\t", "path\t", "mod\\t", "path\\t")):
        return True
    if prefix.endswith(("/", "uri=", "(", "--source-uri ")):
        return False
    if path.endswith(".proto") or (
        path.endswith((".go", ".go.template")) and GO_QUOTED_PATH.fullmatch(prefix)
    ):
        return True
    return path in GO_PATH_FILES


def expired(reason: str, mel: str, entry: dt.date, expiry: dt.date) -> str:
    return f" [expired exception: {reason}; MEL {mel}, {entry} to {expiry}]"


def line_findings(path: str, number: int, line: str, today: dt.date) -> list[str]:
    location = f"{path}:{number}"
    if INLINE_MARKER in line:
        return (
            [] if today <= EXPIRY else [f"{location}:{expired(INLINE_MARKER, 'C', ENTRY, EXPIRY)}"]
        )
    line = URL_ESCAPE.sub(lambda match: URL_DELIMITERS[match.group().lower()], line)
    return reference_findings(path, location, line, today)


def reference_findings(path: str, location: str, line: str, today: dt.date) -> list[str]:
    hits: list[str] = []
    prior_end = 0
    for letter, match in pattern_hits(path, line):
        if match.start() < prior_end:
            continue
        prior_end = match.end()
        reference = match[1] if match.lastindex else match[0]
        if letter == "d" and is_go_identity(path, line[: match.start()], reference):
            continue
        rule = exception(path, line, reference)
        if rule and (rule.expiry is None or today <= rule.expiry):
            continue
        suffix = (
            expired(rule.reason, rule.mel, rule.entry, rule.expiry) if rule and rule.expiry else ""
        )
        hits.append(f"{location}: old organisation reference ({letter}): {reference}{suffix}")
    return hits


def scan(path: str, today: dt.date) -> list[str]:
    source = Path(path)
    if source.is_symlink() or source.is_dir():
        return []
    try:
        with source.open("rb") as stream:
            data = stream.read(MAX_TEXT_BYTES + 1)
        if b"\0" in data[:BINARY_SAMPLE_BYTES]:
            return []
        if len(data) > MAX_TEXT_BYTES:
            codecs.utf_8_decode(data, "strict", False)
            return [f"{path}: tracked text exceeds the {MAX_TEXT_BYTES}-byte limit"]
        content = data.decode("utf-8")
    except UnicodeDecodeError:
        return []
    if allowed(path):
        return permanent_findings(path, today, content)
    findings = []
    for number, line in enumerate(content.splitlines(), 1):
        findings.extend(line_findings(path, number, line, today))
    return findings


def permanent_findings(path: str, today: dt.date, content: str) -> list[str]:
    if today <= EXPIRY:
        return []
    return [
        f"{path}:{number}:{expired(INLINE_MARKER, 'C', ENTRY, EXPIRY)}"
        for number, line in enumerate(content.splitlines(), 1)
        if INLINE_MARKER in line
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="scan all tracked files (the default)")
    parser.add_argument("paths", nargs="*", help="scan only these files")
    args = parser.parse_args()
    if args.all and args.paths:
        parser.error("provide --all or explicit paths")
    try:
        today = dt.date.fromisoformat(os.environ.get("OLD_ORG_REFS_TODAY", str(dt.date.today())))
        paths = args.paths
        if not paths:
            if GIT is None:
                raise ValueError("required executable not found: git")
            result = run_command(
                [GIT, "ls-files", "-z"],
                allowed_executables=(GIT,),
                env=None,
                capture_output=True,
                check=True,
                timeout_seconds=60,
                max_output_bytes=MAX_GIT_OUTPUT_BYTES,
            )
            paths = [os.fsdecode(p) for p in result.stdout.split(b"\0") if p]
        findings = [hit for path in paths for hit in scan(path, today)]
    except (
        ValueError,
        OSError,
        CommandFailed,
        CommandTimedOut,
        CommandOutputLimitExceeded,
        CommandValidationError,
    ) as error:
        print(f"check-old-org-refs: {' '.join(str(error).splitlines())}", file=sys.stderr)
        return 2
    for hit in findings:
        print(hit)
    print(f"check-old-org-refs: {len(findings)} finding(s)", file=sys.stderr)
    return int(bool(findings))


if __name__ == "__main__":
    sys.exit(main())
