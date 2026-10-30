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
import json
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
SYCL_RUNTIME = "tools/rc1-tester/image/sycl-runtime.json"
ONEAPI_BUILD = "scripts/ci/build-oneapi-libvmaf.sh"
# OPERATOR_NODE runs its smoke checks through these two scripts (ADR-0108
# workflow size bound); the assertions on those checks read the scripts.
PUBLISHED_SMOKE = "scripts/release/smoke-published-image.sh"
IMAGE_SMOKE = "scripts/ci/release-image-smoke.sh"
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
        # xxd in the package list of an apt-get install, on its own line or not.
        if re.search(r"apt-get install(?:(?!&&)[\s\S])*?\bxxd\b", body):
            return True
        name = parent
    return False


def lineage(all_stages: dict[str, tuple[str, str]], name: str) -> str:
    """The bodies of a stage and of every stage it is built FROM, in one string."""
    bodies, seen = [], set()
    while name in all_stages and name not in seen:
        seen.add(name)
        parent, body = all_stages[name]
        bodies.append(body)
        name = parent
    return "\n".join(bodies)


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
    oneapi = lineage(gpu_stages, "final-oneapi2026")
    for snippet in (
        # Intel's SYCL runtime files, UMF among them, from the compiler's own
        # installation (ADR-1517).
        "COPY --from=builder-oneapi2026 /stage/lib/intel/ /usr/local/lib/intel/",
        # The compute-runtime GPU driver at INTEL_NEO_VERSION (ADR-1368).
        "install-intel-ocloc.sh --components runtime",
        'ldd "${adapter}" | grep \'not found\'',
    ):
        if snippet not in oneapi:
            raise AssertionError(f"final-oneapi2026 lacks {snippet!r}")
    # The builder stages the runtime through the shared oneAPI build script
    # (HISS-19, ADR-3371); the script holds the staging command.
    if "RUN bash scripts/ci/build-oneapi-libvmaf.sh" not in gpu_stages["builder-oneapi2026"][1]:
        raise AssertionError("builder-oneapi2026 does not run scripts/ci/build-oneapi-libvmaf.sh")
    staging = "prepare_build.py intel-runtime \\\n  tools/rc1-tester/image/sycl-runtime.json"
    if staging not in texts[ONEAPI_BUILD]:
        raise AssertionError("build-oneapi-libvmaf.sh does not stage the runtime files of sycl-runtime.json")
    spec = json.loads(texts[SYCL_RUNTIME])
    if not any("libumf.so.1" in c["names"] for c in spec["components"]):
        raise AssertionError(f"{SYCL_RUNTIME} does not ship UMF, which the adapters load")
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
    # ADR-1513: rclone is built from its release's module source, at the
    # version build-config.env pins, so its corresponding source is exact.
    node_stages = stages(texts["docker/Dockerfile.node"])
    if node_stages.get("rclone-bin", ("", ""))[0] != "${RELEASE_GO_BASE}":
        raise AssertionError("docker/Dockerfile.node has no rclone-bin stage built on ${RELEASE_GO_BASE}")
    if '"github.com/rclone/rclone@${RCLONE_VERSION}"' not in node_stages["rclone-bin"][1]:
        raise AssertionError("the rclone-bin stage does not build rclone from its module source")
    pinned = re.search(r'(?m)^RCLONE_VERSION="(v[0-9.]+)"$', texts["build-config.env"])
    mirrored = re.search(r"(?m)^ARG RCLONE_VERSION=(\S+)$", node_stages["rclone-bin"][1])
    if pinned is None or mirrored is None or pinned.group(1) != mirrored.group(1):
        raise AssertionError("Dockerfile.node's RCLONE_VERSION does not mirror build-config.env")
    if "COPY --from=rclone-bin /usr/local/bin/rclone /usr/local/bin/rclone" not in node_stages["runtime-base"][1]:
        raise AssertionError("the node runtime does not bundle rclone")
    if '--entrypoint /usr/local/bin/rclone "$image" version' not in texts[IMAGE_SMOKE]:
        raise AssertionError(f"{IMAGE_SMOKE}: the node smoke test does not run rclone")
    # ADR-1593: rclone's mount mode needs the setuid fusermount3 and the
    # util-linux mount and umount it runs; the published image lacked them.
    fuse = node_stages.get("fuse-tools", ("", ""))
    if fuse[0] != "${RELEASE_BUILDER_BASE}":
        raise AssertionError("docker/Dockerfile.node has no fuse-tools stage built on ${RELEASE_BUILDER_BASE}")
    for snippet in (
        "--no-install-recommends fuse3 mount",
        "install -D -m 4755 /usr/bin/fusermount3 /fuse-tools/root/usr/bin/fusermount3",
        "install -m 0755 /usr/bin/mount /usr/bin/umount /fuse-tools/root/usr/bin/",
        "record-copied-debian-libs /tmp/fuse-origins.txt /fuse-tools/copied-packages",
    ):
        if snippet not in fuse[1]:
            raise AssertionError(f"the fuse-tools stage lacks {snippet!r}")
    for snippet in (
        "COPY --from=fuse-tools /fuse-tools/root/ /",
        "COPY --from=fuse-tools /fuse-tools/copied-packages/ /usr/local/share/vmafx/fuse-tools/",
    ):
        if snippet not in node_stages["runtime-base"][1]:
            raise AssertionError(f"the node runtime lacks {snippet!r}")
    if "rclone mount :local:/frames /tmp/m --daemon" not in texts[PUBLISHED_SMOKE] or (
        '/usr/bin/fusermount3 -u /tmp/m' not in texts[PUBLISHED_SMOKE]
    ):
        raise AssertionError(f"{PUBLISHED_SMOKE}: the node smoke test does not mount through FUSE")
    if "node) check_mount ;;" not in texts[PUBLISHED_SMOKE]:
        raise AssertionError(f"{PUBLISHED_SMOKE}: the node smoke test skips the FUSE mount")

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
    validate_operator_node_scoring(texts)
    for path in (PRODUCTION, OPERATOR_NODE):
        for block in re.findall(r"(?s)docker run[^\n]*\n(?:[^\n]*\\\n)*[^\n]*" + re.escape(SCORE_CHECK), texts[path]):
            if "--model" in block:
                raise AssertionError(f"{path}: a default-model smoke check passes --model")


def validate_operator_node_scoring(texts: dict[str, str]) -> None:
    """vmafx-server and vmafx-node score with the built-in model, never --model."""
    for kind in ("server", "node"):
        call = (
            r"bash scripts/release/smoke-published-image\.sh\n[^\n]*"
            + kind.upper()
            + r"_IMAGE[^\n]*\n\s+"
            + kind
            + r" "
        )
        if not re.search(call, texts[OPERATOR_NODE]):
            raise AssertionError(f"{OPERATOR_NODE}: vmafx-{kind} is not smoke-tested as {kind}")
    published = texts[PUBLISHED_SMOKE]
    for mapping in ("server) smoke_kind=wrapped ;;", "node) smoke_kind=node ;;"):
        if mapping not in published:
            raise AssertionError(f"{PUBLISHED_SMOKE}: lacks the kind mapping {mapping!r}")
    if 'bash "$REPO_ROOT/scripts/ci/release-image-smoke.sh"' not in published:
        raise AssertionError(f"{PUBLISHED_SMOKE}: does not run {IMAGE_SMOKE}")
    image_smoke = texts[IMAGE_SMOKE]
    if image_smoke.count(SCORE_CHECK) != 1 or "run_score --entrypoint /usr/local/bin/vmaf\n" not in image_smoke:
        raise AssertionError(f"{IMAGE_SMOKE}: wrapped and node images do not score")
    score_args = re.search(r"(?s)score_args=\((.*?)\)", image_smoke)
    if score_args is None or "--model" in score_args.group(1):
        raise AssertionError(f"{IMAGE_SMOKE}: a default-model smoke check passes --model")


def expect_rejected(name: str, texts: dict[str, str]) -> None:
    try:
        validate(texts)
    except AssertionError:
        print(f"PASS: rejected fixture with {name}")
        return
    raise AssertionError(f"broken fixture unexpectedly passed: {name}")


texts = {
    path: (root / path).read_text(encoding="utf-8")
    for path in (
        *DOCKERFILES,
        PRODUCTION,
        OPERATOR_NODE,
        PUBLISHED_SMOKE,
        IMAGE_SMOKE,
        "build-config.env",
        SYCL_RUNTIME,
        ONEAPI_BUILD,
    )
}
validate(texts)

no_xxd = deepcopy(texts)
no_xxd["docker/Dockerfile.production"] = re.sub(
    r"(?m)^\s+xxd \\\n", "", no_xxd["docker/Dockerfile.production"], count=1
)
expect_rejected("a libvmaf builder without xxd", no_xxd)

no_xxd_inline = deepcopy(texts)
no_xxd_inline["docker/Dockerfile.production-gpu"] = no_xxd_inline[
    "docker/Dockerfile.production-gpu"
].replace("pkg-config python3 xxd zstd \\\n", "pkg-config python3 zstd \\\n", 1)
expect_rejected("a libvmaf builder whose one-line package list lacks xxd", no_xxd_inline)

xxd_in_comment = deepcopy(texts)
xxd_in_comment["docker/Dockerfile.production"] = re.sub(
    r"(?m)^\s+xxd \\\n", "", xxd_in_comment["docker/Dockerfile.production"], count=1
).replace("RUN apt-get update", "# xxd comes later\nRUN apt-get update", 1)
expect_rejected("a builder that names xxd only in a comment", xxd_in_comment)

no_umf = deepcopy(texts)
no_umf["build-config.env"] = no_umf["build-config.env"].replace(
    'ONEAPI_RUNTIME_APT_PACKAGES="intel-oneapi-runtime-dpcpp-cpp intel-oneapi-umf"',
    'ONEAPI_RUNTIME_APT_PACKAGES="intel-oneapi-runtime-dpcpp-cpp"',
)
expect_rejected("a oneAPI runtime without UMF", no_umf)

no_staged_umf = deepcopy(texts)
umf_spec = json.loads(no_staged_umf[SYCL_RUNTIME])
umf_spec["components"] = [c for c in umf_spec["components"] if "libumf.so.1" not in c["names"]]
no_staged_umf[SYCL_RUNTIME] = json.dumps(umf_spec)
expect_rejected("a oneAPI image whose staged runtime lacks UMF", no_staged_umf)

unstaged = deepcopy(texts)
unstaged["docker/Dockerfile.production-gpu"] = unstaged["docker/Dockerfile.production-gpu"].replace(
    "COPY --from=builder-oneapi2026 /stage/lib/intel/ /usr/local/lib/intel/\n", ""
)
expect_rejected("a oneAPI image without the staged SYCL runtime", unstaged)

unscripted = deepcopy(texts)
unscripted["docker/Dockerfile.production-gpu"] = unscripted["docker/Dockerfile.production-gpu"].replace(
    "RUN bash scripts/ci/build-oneapi-libvmaf.sh\n", "", 1
)
expect_rejected("a oneAPI builder that skips the shared build script", unscripted)

script_unstaged = deepcopy(texts)
script_unstaged[ONEAPI_BUILD] = script_unstaged[ONEAPI_BUILD].replace(
    "tools/rc1-tester/image/sycl-runtime.json", "", 1
)
expect_rejected("a shared oneAPI build script that stages no SYCL runtime", script_unstaged)

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
with_model[IMAGE_SMOKE] = with_model[IMAGE_SMOKE].replace(
    "--json --output /dev/stdout)", "--model path=/m.json --json --output /dev/stdout)", 1
)
expect_rejected("a default-model check that names a model", with_model)

no_rclone = deepcopy(texts)
no_rclone["docker/Dockerfile.node"] = no_rclone["docker/Dockerfile.node"].replace(
    "COPY --from=rclone-bin /usr/local/bin/rclone /usr/local/bin/rclone\n", ""
)
expect_rejected("a node runtime without rclone", no_rclone)

vendor_rclone = deepcopy(texts)
vendor_rclone["docker/Dockerfile.node"] = vendor_rclone["docker/Dockerfile.node"].replace(
    "FROM ${RELEASE_GO_BASE} AS rclone-bin", "FROM rclone/rclone:1.75.1 AS rclone-bin"
)
expect_rejected("an rclone copied out of the vendor image", vendor_rclone)

no_fuse = deepcopy(texts)
no_fuse["docker/Dockerfile.node"] = no_fuse["docker/Dockerfile.node"].replace(
    "COPY --from=fuse-tools /fuse-tools/root/ /\n", ""
)
expect_rejected("a node runtime without the FUSE tools", no_fuse)

plain_fusermount = deepcopy(texts)
plain_fusermount["docker/Dockerfile.node"] = plain_fusermount["docker/Dockerfile.node"].replace(
    "install -D -m 4755 /usr/bin/fusermount3", "install -D -m 0755 /usr/bin/fusermount3"
)
expect_rejected("a fusermount3 without its setuid bit", plain_fusermount)

no_mount_smoke = deepcopy(texts)
no_mount_smoke[PUBLISHED_SMOKE] = no_mount_smoke[PUBLISHED_SMOKE].replace(
    "/usr/bin/fusermount3 -u /tmp/m", "true"
)
expect_rejected("a node smoke test without a FUSE mount", no_mount_smoke)

mount_skipped = deepcopy(texts)
mount_skipped[PUBLISHED_SMOKE] = mount_skipped[PUBLISHED_SMOKE].replace(
    "node) check_mount ;;", "node) ;;"
)
expect_rejected("a node smoke test that never calls the FUSE mount", mount_skipped)

node_as_operator = deepcopy(texts)
node_as_operator[OPERATOR_NODE] = re.sub(
    r"(NODE_IMAGE[^\n]*\n\s+)node ", r"\1operator ", node_as_operator[OPERATOR_NODE], count=1
)
expect_rejected("a vmafx-node smoke test run as the score-less operator kind", node_as_operator)

server_unscored = deepcopy(texts)
server_unscored[PUBLISHED_SMOKE] = server_unscored[PUBLISHED_SMOKE].replace(
    "server) smoke_kind=wrapped ;;", "server) smoke_kind=binary ;;"
)
expect_rejected("a vmafx-server smoke test that maps to the score-less binary kind", server_unscored)

drifted_rclone = deepcopy(texts)
drifted_rclone["build-config.env"] = drifted_rclone["build-config.env"].replace(
    'RCLONE_VERSION="v1.75.1"', 'RCLONE_VERSION="v1.75.2"'
)
expect_rejected("an rclone version that does not mirror build-config.env", drifted_rclone)

print(
    "PASS: images build their models in, the oneAPI image carries its pinned GPU runtime, "
    "the node bundles rclone and its FUSE tools, and the smoke tests load and mount them"
)
PY
