#!/usr/bin/env bash
# Regression test for protected publication environments and exact tag identities.
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
workflow_paths = (
    ".github/workflows/supply-chain.yml",
    ".github/workflows/docker-publish-production.yml",
    ".github/workflows/docker-publish-operator-node.yml",
)


def jobs(text: str) -> dict[str, str]:
    _, jobs_text = text.split("\njobs:\n", maxsplit=1)
    matches = list(re.finditer(r"(?m)^  ([a-z0-9-]+):\n", jobs_text))
    return {
        match.group(1): jobs_text[
            match.end() : matches[index + 1].start()
            if index + 1 < len(matches)
            else len(jobs_text)
        ]
        for index, match in enumerate(matches)
    }


parsed = {}
for relative_path in workflow_paths:
    parsed[relative_path] = jobs((root / relative_path).read_text(encoding="utf-8"))

for relative_path, blocks in parsed.items():
    validation = blocks["validate-release"]
    if "    environment:" in validation:
        raise AssertionError(f"{relative_path}: read-only validation is environment-gated")

    for job, block in blocks.items():
        write_scopes = re.findall(r"(?m)^      ([a-z0-9-]+): write(?:\s|$)", block)
        if not write_scopes:
            continue
        if job in {"slsa-provenance", "mcp-slsa-provenance"}:
            if write_scopes != ["id-token"]:
                raise AssertionError(
                    f"{relative_path}: {job} has unexpected write scopes {write_scopes}"
                )
            if "      contents: read\n" not in block:
                raise AssertionError(f"{relative_path}: {job} may write repository contents")
            if "      upload-assets: false\n" not in block:
                raise AssertionError(f"{relative_path}: {job} may upload a release asset")
            if "upload-tag-name:" in block:
                raise AssertionError(f"{relative_path}: {job} retains direct release upload")
            continue

        expected = "pypi-publish" if job == "mcp-publish-pypi" else "release-publish"
        if not re.search(
            rf"(?m)^    environment:\s+{re.escape(expected)}(?:\s+#.*)?$", block
        ):
            raise AssertionError(
                f"{relative_path}: write-bearing job {job} is not bound to {expected}"
            )

supply = (root / workflow_paths[0]).read_text(encoding="utf-8")
for name in (
    "vmafx-build-provenance.intoto.jsonl",
    "vmaf-mcp-provenance.intoto.jsonl",
):
    artifact_path = f"dist/{name}/{name}"
    if supply.count(artifact_path) < 2:
        raise AssertionError(f"supply-chain.yml does not require and attach {artifact_path}")

for relative_path in workflow_paths[1:]:
    text = (root / relative_path).read_text(encoding="utf-8")
    if "--certificate-identity-regexp" in text or "@.*" in text:
        raise AssertionError(f"{relative_path}: broad certificate identity remains")
    identity = (
        "https://github.com/VMAFx/vmafx/.github/workflows/"
        f"{Path(relative_path).name}@refs/tags/${{PUBLISH_TAG}}"
    )
    if identity not in text:
        raise AssertionError(f"{relative_path}: exact release-tag identity is absent")

print("PASS: every publication write is environment-bound")
print("PASS: SLSA provenance is attached only by the protected release writer")
print("PASS: container signatures require the exact published tag identity")
PY
