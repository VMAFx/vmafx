#!/usr/bin/env bash
# Regression test for release-tag/source binding in production image workflows.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"

python3 - "$REPO_ROOT" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
# Job names carry the SDK generation ("build-rocm7" -> "build-rocm10",
# "build-oneapi2025" -> "build-oneapi2026"), so they change every time a vendor
# SDK is bumped. Matching the exact name made this test fail on the rename
# rather than on anything it is actually checking. Match the stable prefix and
# resolve the real job name from the workflow instead; the assertion that
# matters is that exactly one such build job exists and is source-bound.
workflows = {
    ".github/workflows/docker-publish-production.yml": (
        "build-cpu",
        "build-cuda",
        "build-rocm",
        "build-oneapi",
        "build-server",
    ),
    ".github/workflows/docker-publish-operator-node.yml": (
        "build-operator",
        "build-server",
        "build-node",
    ),
}

validation_snippets = (
    'expected_ref="refs/tags/$PUBLISH_TAG"',
    '"$GITHUB_REF" != "$expected_ref"',
    'scripts/release/verify-release-version.sh "$PUBLISH_TAG"',
    '"$(git rev-parse HEAD)" != "$GITHUB_SHA"',
    'releases/tags/$PUBLISH_TAG',
    ".published_at != null",
)


def job_block(jobs_text: str, job_prefix: str) -> str:
    """Return the body of the single job whose name starts with job_prefix."""
    matches = list(
        re.finditer(
            rf"(?ms)^  ({re.escape(job_prefix)}[a-z0-9-]*):\n(.*?)(?=^  [a-z0-9-]+:\n|\Z)",
            jobs_text,
        )
    )
    if not matches:
        raise AssertionError(f"missing job matching {job_prefix}*")
    if len(matches) > 1:
        found = ", ".join(m.group(1) for m in matches)
        raise AssertionError(f"{job_prefix}* is ambiguous: matched {found}")
    return matches[0].group(2)


for relative_path, build_jobs in workflows.items():
    text = (root / relative_path).read_text(encoding="utf-8")
    head, jobs_text = text.split("\njobs:\n", maxsplit=1)
    if not re.search(
        r"(?ms)^  workflow_dispatch:\n.*?^      tag:\n.*?^        required: true$",
        head,
    ):
        raise AssertionError(f"{relative_path}: manual tag input is not required")
    if "default: \"dev\"" in head or "|| 'dev'" in head:
        raise AssertionError(f"{relative_path}: arbitrary dev publication remains enabled")

    validation = job_block(jobs_text, "validate-release")
    for snippet in validation_snippets:
        if snippet not in validation:
            raise AssertionError(
                f"{relative_path}: validate-release missing {snippet!r}"
            )

    for job in build_jobs:
        block = job_block(jobs_text, job)
        if "    needs: validate-release\n" not in block:
            raise AssertionError(f"{relative_path}: {job} bypasses validate-release")
        if "ref: ${{ needs.validate-release.outputs.tag }}" not in block:
            raise AssertionError(f"{relative_path}: {job} does not check out validated tag")

    if "ref: ${{ github.event.release.tag_name || github.ref }}" in jobs_text:
        raise AssertionError(f"{relative_path}: independent event-ref checkout remains")
    print(f"PASS: {relative_path} binds every image build to one published tag")
PY
