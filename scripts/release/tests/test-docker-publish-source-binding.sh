#!/usr/bin/env bash
# Regression test for release-tag/source binding in production image workflows.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

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
    # ADR-1347: only a recovery dispatch on the default branch may run away
    # from the tag, and tag runs still bind the workflow source to the tag.
    '"$GITHUB_EVENT_NAME" != workflow_dispatch',
    '"$GITHUB_REF" != "refs/heads/$DEFAULT_BRANCH"',
    '"$recovery" == false && "$(git rev-parse HEAD)" != "$GITHUB_SHA"',
    'echo "source_sha=$(git rev-parse HEAD)"',
)

RECOVERY_OVERLAY = (
    "        if: needs.validate-release.outputs.recovery == 'true'\n",
    '          git checkout FETCH_HEAD -- docker/ Dockerfile.go-server ffmpeg-patches/\n',
)

# The OCI revision label names the packaged source (the tag's commit), not the
# recipe commit a recovery run executes at (v1.0.0-rc.1's first recovered
# images said a919f359 instead of ce00cf24).
SOURCE_REVISION_LABEL = (
    "            org.opencontainers.image.revision="
    "${{ needs.validate-release.outputs.source-sha }}\n"
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
    if "github.event.release.prerelease" in validation:
        raise AssertionError(
            f"{relative_path}: prerelease must come from the release API; a dispatch has no release payload"
        )
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
        # The recovery overlay replaces only the build recipe, only in recovery.
        for snippet in RECOVERY_OVERLAY:
            if snippet not in block:
                raise AssertionError(f"{relative_path}: {job} recovery overlay missing {snippet!r}")
        if SOURCE_REVISION_LABEL not in block:
            raise AssertionError(f"{relative_path}: {job} does not label the tag's source revision")
        overlays = re.findall(r"git checkout FETCH_HEAD -- ([^\n]*)", block)
        # ADR-1350: the recipe includes the patches to the bundled FFmpeg.
        if overlays != ["docker/ Dockerfile.go-server ffmpeg-patches/"]:
            raise AssertionError(f"{relative_path}: {job} overlays {overlays}, not the build recipe only")

    if relative_path.endswith("docker-publish-operator-node.yml"):
        # ADR-1349: every Go service image builds each architecture on a native
        # runner and merges the two digests; under QEMU the arm64 builds outran
        # (node) or neared (server, operator) their limits.
        for image, env_name in (("node", "NODE_IMAGE"), ("server", "SERVER_IMAGE"), ("operator", "OPERATOR_IMAGE")):
            build = job_block(jobs_text, f"build-{image}")
            for snippet in (
                "runs-on: ${{ matrix.runner }}",
                "runner: ubuntu-26.04-arm",
                "push-by-digest=true",
            ):
                if snippet not in build:
                    raise AssertionError(f"{relative_path}: build-{image} lacks {snippet!r}")
            if "setup-qemu-action" in build:
                raise AssertionError(f"{relative_path}: build-{image} still emulates arm64 with QEMU")
            publish = job_block(jobs_text, f"publish-{image}")
            for snippet in (
                f"      - build-{image}\n",
                'if [[ "${#digests[@]}" -ne 2 ]]; then',
                "docker buildx imagetools create",
                "${{ env." + env_name + " }}@${{ steps.index.outputs.digest }}",
            ):
                if snippet not in publish:
                    raise AssertionError(f"{relative_path}: publish-{image} lacks {snippet!r}")

    if "ref: ${{ github.event.release.tag_name || github.ref }}" in jobs_text:
        raise AssertionError(f"{relative_path}: independent event-ref checkout remains")
    print(f"PASS: {relative_path} binds every image build to one published tag")
PY
