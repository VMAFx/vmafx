#!/usr/bin/env bash
# Regression tests for what the published container images must be able to do
# at run time, beyond starting.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# The first v1.0.0-rc.1 images passed every publication smoke test and were
# still broken in two ways that `vmaf --version` cannot see:
#   * the CPU, MCP-server and CUDA images had no built-in models: their
#     builders lacked xxd, and core/src/meson.build skips the model embed
#     without an error when xxd is missing, so scoring without --model failed;
#   * the oneAPI image found no SYCL device: its Unified Runtime adapters need
#     libumf.so.1, which intel/oneapi-runtime does not ship.
# This test pins the recipe fixes and the smoke checks that would have caught
# both, and proves each assertion rejects the defect it guards against.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../../.." && pwd)"

python3 - "$REPO_ROOT" <<'PY'
import re
import sys
from copy import deepcopy
from pathlib import Path

root = Path(sys.argv[1])
DOCKERFILES = (
    "docker/Dockerfile.production",
    "docker/Dockerfile.production-gpu",
    "docker/Dockerfile.node",
    "docker/Dockerfile.controller",
    "Dockerfile.go-server",
)
PRODUCTION = ".github/workflows/docker-publish-production.yml"
OPERATOR_NODE = ".github/workflows/docker-publish-operator-node.yml"
SCORE_CHECK = "jq -e '.pooled_metrics.vmaf.mean'"


def stages(text: str) -> dict[str, tuple[str, str]]:
    """Map stage name -> (parent image or stage, body)."""
    found = {}
    parts = re.split(r"(?m)^FROM\s+(\S+)\s+AS\s+(\S+)\s*$", text)
    for index in range(1, len(parts) - 1, 3):
        found[parts[index + 1]] = (parts[index], parts[index + 2])
    return found


def has_xxd(all_stages: dict[str, tuple[str, str]], name: str) -> bool:
    seen = set()
    while name in all_stages and name not in seen:
        seen.add(name)
        parent, body = all_stages[name]
        if re.search(r"(?m)^\s+xxd\b", body):
            return True
        name = parent
    return False


def validate(texts: dict[str, str]) -> None:
    for path in DOCKERFILES:
        all_stages = stages(texts[path])
        for name, (_parent, body) in all_stages.items():
            if "meson setup" in body and not has_xxd(all_stages, name):
                raise AssertionError(
                    f"{path}: stage {name} builds libvmaf without xxd, so it "
                    "silently embeds no built-in model"
                )

    gpu_stages = stages(texts["docker/Dockerfile.production-gpu"])
    oneapi = gpu_stages["final-oneapi2025"][1]
    for snippet in (
        '"${INTEL_UMF_RUNTIME_PACKAGE}"',
        "/opt/intel/oneapi/umf/1.0/lib",
        'ldd "${adapter}" | grep \'not found\'',
    ):
        if snippet not in oneapi:
            raise AssertionError(f"final-oneapi2025 lacks {snippet!r}")
    config = texts["build-config.env"]
    if not re.search(r'(?m)^INTEL_UMF_RUNTIME_PACKAGE="intel-oneapi-umf-1\.0=', config):
        raise AssertionError("build-config.env does not pin the UMF 1.0 runtime package")

    # pkg/storage runs rclone for remote inputs (ADR-0719); the v1.0.0-rc.1
    # node image shipped without it although the storage guide promised it.
    node_stages = stages(texts["docker/Dockerfile.node"])
    if node_stages.get("rclone-bin", ("", ""))[0] != "${RCLONE_IMAGE}":
        raise AssertionError("docker/Dockerfile.node has no rclone-bin stage from ${RCLONE_IMAGE}")
    if "COPY --from=rclone-bin /usr/local/bin/rclone /usr/local/bin/rclone" not in node_stages["runtime-base"][1]:
        raise AssertionError("the node runtime does not bundle rclone")
    if '--entrypoint /usr/local/bin/rclone "${image}" version' not in texts[OPERATOR_NODE]:
        raise AssertionError(f"{OPERATOR_NODE}: the node smoke test does not run rclone")

    production = texts[PRODUCTION]
    if production.count(SCORE_CHECK) != 3:
        raise AssertionError(
            f"{PRODUCTION}: expected default-model scoring in the CPU, GPU and "
            f"MCP smoke tests, found {production.count(SCORE_CHECK)}"
        )
    if "libur_adapter_*.so.0" not in production or "needs.build-oneapi2025.outputs.digest" not in production:
        raise AssertionError(f"{PRODUCTION}: the oneAPI adapter check is missing")
    if texts[OPERATOR_NODE].count(SCORE_CHECK) != 2:
        raise AssertionError(
            f"{OPERATOR_NODE}: expected default-model scoring for vmafx-server and vmafx-node"
        )
    for path in (PRODUCTION, OPERATOR_NODE):
        for block in re.findall(r"(?s)docker run[^\n]*\n(?:[^\n]*\\\n)*[^\n]*" + re.escape(SCORE_CHECK), texts[path]):
            if "--model" in block:
                raise AssertionError(f"{path}: a default-model smoke check passes --model")


def expect_rejected(name: str, texts: dict[str, str]) -> None:
    try:
        validate(texts)
    except AssertionError:
        print(f"PASS: rejected fixture with {name}")
        return
    raise AssertionError(f"broken fixture unexpectedly passed: {name}")


texts = {
    path: (root / path).read_text(encoding="utf-8")
    for path in (*DOCKERFILES, PRODUCTION, OPERATOR_NODE, "build-config.env")
}
validate(texts)

no_xxd = deepcopy(texts)
no_xxd["docker/Dockerfile.production"] = re.sub(
    r"(?m)^\s+xxd \\\n", "", no_xxd["docker/Dockerfile.production"], count=1
)
expect_rejected("a libvmaf builder without xxd", no_xxd)

no_umf = deepcopy(texts)
no_umf["docker/Dockerfile.production-gpu"] = no_umf["docker/Dockerfile.production-gpu"].replace(
    '"${INTEL_UMF_RUNTIME_PACKAGE}"', '"intel-oneapi-runtime-dpcpp-cpp"'
)
expect_rejected("a oneAPI runtime without UMF", no_umf)

version_only = deepcopy(texts)
version_only[PRODUCTION] = version_only[PRODUCTION].replace(SCORE_CHECK, "true", 1)
expect_rejected("a smoke test that only runs --version", version_only)

with_model = deepcopy(texts)
with_model[OPERATOR_NODE] = with_model[OPERATOR_NODE].replace(
    "--json --output /dev/stdout", "--model path=/m.json --json --output /dev/stdout", 1
)
expect_rejected("a default-model check that names a model", with_model)

no_rclone = deepcopy(texts)
no_rclone["docker/Dockerfile.node"] = no_rclone["docker/Dockerfile.node"].replace(
    "COPY --from=rclone-bin /usr/local/bin/rclone /usr/local/bin/rclone\n", ""
)
expect_rejected("a node runtime without rclone", no_rclone)

print("PASS: images build their models in, the node bundles rclone, and the smoke tests load them")
PY
