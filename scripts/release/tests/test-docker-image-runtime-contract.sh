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
# The v1.0.0-rc.2 oneAPI image then segfaulted on every Arc B580 SYCL run: its
# compute-runtime GPU driver was the 25.18 one intel/oneapi-runtime:2025.3.1
# carried (ADR-1368).
# This test pins the recipe fixes and the smoke checks that would have caught
# them, and proves each assertion rejects the defect it guards against.

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
    oneapi = gpu_stages["final-oneapi2026"][1]
    for snippet in (
        # Intel's SYCL runtime with UMF, at the compiler's exact apt build.
        "install-intel-oneapi.sh --mode=runtime",
        # The compute-runtime GPU driver at INTEL_NEO_VERSION (ADR-1368).
        "install-intel-ocloc.sh --components runtime",
        "/opt/intel/oneapi/umf/latest/lib",
        'ldd "${adapter}" | grep \'not found\'',
    ):
        if snippet not in oneapi:
            raise AssertionError(f"final-oneapi2026 lacks {snippet!r}")
    if gpu_stages.get("final-oneapi2025", ("", ""))[0] != "final-oneapi2026":
        raise AssertionError("final-oneapi2025 is no longer an alias of final-oneapi2026")
    config = texts["build-config.env"]
    runtime_packages = re.search(r'(?m)^ONEAPI_RUNTIME_APT_PACKAGES="([^"]*)"', config)
    if runtime_packages is None or "intel-oneapi-umf" not in runtime_packages.group(1).split():
        raise AssertionError("build-config.env does not install UMF with the oneAPI runtime")
    if not re.search(r'(?m)^ONEAPI_UMF_APT_VERSION="[0-9.]+-[0-9]+"', config):
        raise AssertionError("build-config.env does not pin the UMF runtime package")

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
    if "libur_adapter_*.so.0" not in production or "needs.build-oneapi2026.outputs.digest" not in production:
        raise AssertionError(f"{PRODUCTION}: the oneAPI adapter check is missing")
    # HISS-14: the pre-2026 tag suffix keeps resolving to the same image.
    for suffix in ("-oneapi2026", "-oneapi2025"):
        if f"type=raw,value=${{{{ env.PUBLISH_TAG }}}}{suffix}" not in production:
            raise AssertionError(f"{PRODUCTION}: the oneAPI image is not tagged {suffix}")
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
no_umf["build-config.env"] = no_umf["build-config.env"].replace(
    'ONEAPI_RUNTIME_APT_PACKAGES="intel-oneapi-runtime-dpcpp-cpp intel-oneapi-umf"',
    'ONEAPI_RUNTIME_APT_PACKAGES="intel-oneapi-runtime-dpcpp-cpp"',
)
expect_rejected("a oneAPI runtime without UMF", no_umf)

old_driver = deepcopy(texts)
old_driver["docker/Dockerfile.production-gpu"] = old_driver[
    "docker/Dockerfile.production-gpu"
].replace("    && bash /tmp/vmafx/scripts/ci/install-intel-ocloc.sh --components runtime /tmp/vmafx \\\n", "")
expect_rejected("a oneAPI runtime without the pinned GPU driver", old_driver)

no_alias = deepcopy(texts)
no_alias["docker/Dockerfile.production-gpu"] = no_alias["docker/Dockerfile.production-gpu"].replace(
    "FROM final-oneapi2026 AS final-oneapi2025\n", ""
)
expect_rejected("a dropped final-oneapi2025 stage alias", no_alias)

no_old_tag = deepcopy(texts)
no_old_tag[PRODUCTION] = no_old_tag[PRODUCTION].replace(
    "type=raw,value=${{ env.PUBLISH_TAG }}-oneapi2025", "", 1
)
expect_rejected("a dropped -oneapi2025 tag", no_old_tag)

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

print(
    "PASS: images build their models in, the oneAPI image carries its pinned GPU runtime, "
    "the node bundles rclone, and the smoke tests load them"
)
PY
