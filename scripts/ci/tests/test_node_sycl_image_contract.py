#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for the vmafx-node SYCL container image (ADR-3371).

Asserts that:
1. Dockerfile.node stage node-sycl / node-sycl-assembled copies libvmaf, vmaf, and Intel
   runtime assets from builder-oneapi2026, never falling back to vmaf-builder.
2. Both Dockerfile.node and Dockerfile.production-gpu delegate the oneAPI compilation
   and staging to the shared scripts/ci/build-oneapi-libvmaf.sh script (HISS-19).
3. Neither Dockerfile contains an inline '-Denable_sycl=true' Meson invocation.
4. node-sycl declares ENV VMAFX_BACKEND=sycl.
5. The node-sycl notices and licence check read the scan of the oneAPI build
   (stage go-licences-sycl), never the CPU build's scan of go-builder.
6. node-sycl-assembled copies the Intel GPU user-mode stack (stage
   intel-gpu-runtime-libs, Level Zero loader and GPU driver), the node-sycl build
   orders the runtime check that resolves the Unified Runtime adapters, and the
   licence manifest records component intel-gpu-stack for production-node-sycl-image.
7. Planted negatives (vmaf-builder fallback, VMAFX_BACKEND=cpu, inline meson flags,
   CPU licence scan, missing GPU stack, missing runtime check) fail.
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
DOCKERFILE_NODE = ROOT / "docker" / "Dockerfile.node"
DOCKERFILE_PROD_GPU = ROOT / "docker" / "Dockerfile.production-gpu"
SHARED_SCRIPT_PATH = ROOT / "scripts" / "ci" / "build-oneapi-libvmaf.sh"
LICENSING_JSON = ROOT / "tools" / "rc1-tester" / "image" / "licensing.json"
GPU_STACK_LIBS = ("libze_loader.so.1", "libze_intel_gpu.so.1")

FROM_RE = re.compile(
    r"(?im)^FROM\s+(?:--platform=\S+\s+)?(?P<parent>\S+)(?:\s+AS\s+(?P<name>\S+))?\s*$"
)
COPY_FROM_RE = re.compile(r"(?im)^\s*COPY\s+--from=(?P<source>\S+)\s+(?P<src>\S+)\s+(?P<dst>\S+)")
ENV_BACKEND_RE = re.compile(r"(?im)^\s*ENV\s+VMAFX_BACKEND=(?P<backend>\S+)")
INLINE_SYCL_MESON_RE = re.compile(r"(?m)^[^#\n]*meson\s+setup\b[^\n]*-Denable_sycl=true")
SHARED_SCRIPT_INVOCATION_RE = re.compile(
    r"(?m)^[^#\n]*\b(?:bash\s+)?scripts/ci/build-oneapi-libvmaf\.sh\b"
)


def parse_stages(text: str) -> dict[str, str]:
    """Extract Dockerfile stages as a mapping of stage_name -> stage_body."""
    matches = list(FROM_RE.finditer(text))
    stages: dict[str, str] = {}
    for idx, match in enumerate(matches):
        end = matches[idx + 1].start() if idx + 1 < len(matches) else len(text)
        name = (match.group("name") or f"stage-{idx}").lower()
        stages[name] = text[match.end() : end]
    return stages


def validate_shared_script_usage(dockerfile_node: str, dockerfile_prod_gpu: str) -> list[str]:
    """Verify both Dockerfiles invoke the shared oneAPI script and avoid inline flags."""
    errors: list[str] = []

    if not SHARED_SCRIPT_INVOCATION_RE.search(dockerfile_node):
        errors.append("Dockerfile.node does not invoke scripts/ci/build-oneapi-libvmaf.sh")
    if not SHARED_SCRIPT_INVOCATION_RE.search(dockerfile_prod_gpu):
        errors.append(
            "Dockerfile.production-gpu does not invoke scripts/ci/build-oneapi-libvmaf.sh"
        )

    for name, content in [
        ("Dockerfile.node", dockerfile_node),
        ("Dockerfile.production-gpu", dockerfile_prod_gpu),
    ]:
        if INLINE_SYCL_MESON_RE.search(content):
            errors.append(
                f"{name} duplicates inline meson setup -Denable_sycl=true "
                f"(must be in build-oneapi-libvmaf.sh only)"
            )
    return errors


def validate_builder_stage(stages: dict[str, str]) -> list[str]:
    """Check builder-oneapi2026 presence and invocation."""
    if "builder-oneapi2026" not in stages:
        return ["Dockerfile.node is missing stage 'builder-oneapi2026'"]
    builder_body = stages["builder-oneapi2026"]
    if not SHARED_SCRIPT_INVOCATION_RE.search(builder_body):
        return ["stage builder-oneapi2026 must invoke scripts/ci/build-oneapi-libvmaf.sh"]
    return []


def inspect_vmaf_copies(body: str) -> tuple[bool, bool]:
    """Scan stage body for vmaf COPY sources: (from_builder, from_fallback)."""
    from_builder = False
    from_fallback = False
    for match in COPY_FROM_RE.finditer(body):
        source = match.group("source").lower()
        src = match.group("src")
        if "vmaf" in src:
            if source == "builder-oneapi2026":
                from_builder = True
            elif source == "vmaf-builder":
                from_fallback = True
    return from_builder, from_fallback


def validate_copy_sources(stages: dict[str, str]) -> list[str]:
    """Check node-sycl copies vmaf exclusively from builder-oneapi2026."""
    target_stages = [s for s in ["node-sycl-assembled", "node-sycl"] if s in stages]
    if not target_stages:
        return ["Dockerfile.node missing node-sycl or node-sycl-assembled stage"]

    from_builder = False
    from_fallback = False
    for stage_name in target_stages:
        b, f = inspect_vmaf_copies(stages[stage_name])
        from_builder = from_builder or b
        from_fallback = from_fallback or f

    if from_fallback:
        return [
            "node-sycl copies libvmaf from vmaf-builder (-Denable_sycl=false); "
            "must copy from builder-oneapi2026"
        ]
    if not from_builder:
        return ["node-sycl does not copy libvmaf/vmaf from builder-oneapi2026"]
    return []


def validate_backend_env(stages: dict[str, str]) -> list[str]:
    """Check ENV VMAFX_BACKEND=sycl declaration."""
    backend_val: str | None = None
    for stage_name in ["node-sycl-assembled", "node-sycl"]:
        if stage_name in stages:
            match = ENV_BACKEND_RE.search(stages[stage_name])
            if match:
                backend_val = match.group("backend").strip()

    if backend_val != "sycl":
        return [f"node-sycl must declare ENV VMAFX_BACKEND=sycl (got '{backend_val}')"]
    return []


LICENCE_BIND_RE = re.compile(r"--mount=type=bind,from=(?P<source>[^,\s]+),source=/licence-inputs")
LICENCE_STAGES = ("node-sycl-notices", "node-sycl-licence-check")


def validate_licence_scan(stages: dict[str, str]) -> list[str]:
    """Check the node-sycl licence stages read the oneAPI build's scan."""
    errors: list[str] = []
    if "go-licences-sycl" not in stages:
        errors.append("Dockerfile.node is missing stage 'go-licences-sycl'")
    elif "--from=builder-oneapi2026 /licence-inputs/" not in stages["go-licences-sycl"]:
        errors.append("stage go-licences-sycl must merge builder-oneapi2026's scan")
    for name in LICENCE_STAGES:
        sources = [m.group("source") for m in LICENCE_BIND_RE.finditer(stages.get(name, ""))]
        if sources != ["go-licences-sycl"]:
            errors.append(
                f"stage {name} must bind /licence-inputs from go-licences-sycl (got {sources})"
            )
    return errors


def validate_gpu_stack(stages: dict[str, str]) -> list[str]:
    """Check the Intel GPU user-mode stack reaches the image and is resolved."""
    errors: list[str] = []
    gpu_stage = stages.get("intel-gpu-runtime-libs", "")
    if not gpu_stage:
        errors.append("Dockerfile.node is missing stage 'intel-gpu-runtime-libs'")
    else:
        if "install-intel-ocloc.sh --components runtime" not in gpu_stage:
            errors.append("stage intel-gpu-runtime-libs must run install-intel-ocloc.sh runtime")
        for lib in GPU_STACK_LIBS:
            if lib not in gpu_stage:
                errors.append(f"stage intel-gpu-runtime-libs must check {lib}")
        # licensing reads file ownership from status.d/<pkg>.md5sums; gmmlib's
        # package ships none, so the stage writes every list and refuses an empty one.
        if '> "/out/var/lib/dpkg/status.d/${pkg}.md5sums"' not in gpu_stage or (
            'test -s "/out/var/lib/dpkg/status.d/${pkg}.md5sums"' not in gpu_stage
        ):
            errors.append("stage intel-gpu-runtime-libs must write and check each md5sums list")
    sources = [
        m.group("source") for m in COPY_FROM_RE.finditer(stages.get("node-sycl-assembled", ""))
    ]
    if "intel-gpu-runtime-libs" not in sources:
        errors.append("node-sycl-assembled does not copy the Intel GPU stack")
    check = stages.get("node-sycl-runtime-check", "")
    if "libur_adapter_" not in check or "not found" not in check:
        errors.append("stage node-sycl-runtime-check must resolve the Unified Runtime adapters")
    if "from=node-sycl-runtime-check" not in stages.get("node-sycl", ""):
        errors.append("stage node-sycl does not order node-sycl-runtime-check")
    return errors


def validate_licence_manifest(manifest: dict[str, Any]) -> list[str]:
    """Check production-node-sycl-image records the Intel GPU stack."""
    record = manifest.get("artifacts", {}).get("production-node-sycl-image", {})
    refs = [(c.get("from"), c.get("id")) for c in record.get("components", [])]
    if ("sycl-image", "intel-gpu-stack") not in refs:
        return ["production-node-sycl-image does not record component intel-gpu-stack"]
    return []


def validate_node_sycl_stages(dockerfile_node: str) -> list[str]:
    """Validate node-sycl stages in Dockerfile.node conform to ADR-3371."""
    stages = parse_stages(dockerfile_node)
    errors: list[str] = []
    errors.extend(validate_builder_stage(stages))
    if errors:
        return errors
    errors.extend(validate_copy_sources(stages))
    errors.extend(validate_backend_env(stages))
    errors.extend(validate_licence_scan(stages))
    errors.extend(validate_gpu_stack(stages))
    return errors


def validate_node_sycl_contract(dockerfile_node: str, dockerfile_prod_gpu: str) -> list[str]:
    """Run full suite of validation checks."""
    errors: list[str] = []
    errors.extend(validate_shared_script_usage(dockerfile_node, dockerfile_prod_gpu))
    errors.extend(validate_node_sycl_stages(dockerfile_node))
    return errors


class NodeSyclImageContractTest(unittest.TestCase):
    """Positive, negative, and boundary tests for the node-sycl image contract."""

    def test_shared_script_exists_and_executable(self) -> None:
        self.assertTrue(
            SHARED_SCRIPT_PATH.is_file(),
            f"Expected shared script at {SHARED_SCRIPT_PATH}",
        )
        self.assertTrue(
            bool(SHARED_SCRIPT_PATH.stat().st_mode & 0o111),
            "scripts/ci/build-oneapi-libvmaf.sh must be executable",
        )

    def test_real_dockerfiles_comply_with_contract(self) -> None:
        node_text = DOCKERFILE_NODE.read_text(encoding="utf-8")
        gpu_text = DOCKERFILE_PROD_GPU.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(node_text, gpu_text)
        self.assertEqual(errors, [])

    def test_planted_negative_copies_from_vmaf_builder(self) -> None:
        """Planted negative: node-sycl copies from vmaf-builder instead of oneAPI builder."""
        bad_node = """
FROM debian:13 AS builder-oneapi2026
RUN bash scripts/ci/build-oneapi-libvmaf.sh

FROM debian:13 AS node-sycl-assembled
COPY --from=vmaf-builder /dist/lib/libvmaf.so /usr/local/lib/
COPY --from=vmaf-builder /dist/bin/vmaf /opt/vmafx/bin/vmaf
ENV VMAFX_BACKEND=sycl

FROM scratch AS node-sycl
COPY --from=node-sycl-assembled / /
"""
        dummy_gpu = "RUN bash scripts/ci/build-oneapi-libvmaf.sh\n"
        errors = validate_node_sycl_contract(bad_node, dummy_gpu)
        self.assertTrue(
            any("copies libvmaf from vmaf-builder" in err for err in errors),
            f"Expected fallback to vmaf-builder error, got: {errors}",
        )

    def test_planted_negative_backend_is_cpu(self) -> None:
        """Planted negative: node-sycl declares VMAFX_BACKEND=cpu."""
        bad_node = """
FROM debian:13 AS builder-oneapi2026
RUN bash scripts/ci/build-oneapi-libvmaf.sh

FROM debian:13 AS node-sycl-assembled
COPY --from=builder-oneapi2026 /dist/lib/libvmaf.so /usr/local/lib/
COPY --from=builder-oneapi2026 /dist/bin/vmaf /opt/vmafx/bin/vmaf
ENV VMAFX_BACKEND=cpu

FROM scratch AS node-sycl
COPY --from=node-sycl-assembled / /
"""
        dummy_gpu = "RUN bash scripts/ci/build-oneapi-libvmaf.sh\n"
        errors = validate_node_sycl_contract(bad_node, dummy_gpu)
        self.assertTrue(
            any("ENV VMAFX_BACKEND=sycl" in err for err in errors),
            f"Expected VMAFX_BACKEND error, got: {errors}",
        )

    def test_planted_negative_inline_enable_sycl_meson(self) -> None:
        """Planted negative: Dockerfile duplicates meson setup -Denable_sycl=true inline."""
        bad_gpu = """
RUN meson setup core/build core -Denable_sycl=true
RUN bash scripts/ci/build-oneapi-libvmaf.sh
"""
        valid_node = DOCKERFILE_NODE.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(valid_node, bad_gpu)
        self.assertTrue(
            any("duplicates inline meson setup -Denable_sycl=true" in err for err in errors),
            f"Expected inline meson error, got: {errors}",
        )

    def test_planted_negative_missing_shared_script_invocation(self) -> None:
        """Planted negative: Dockerfile.node omits build-oneapi-libvmaf.sh call."""
        bad_node = """
FROM debian:13 AS builder-oneapi2026
RUN echo "custom build"

FROM debian:13 AS node-sycl-assembled
COPY --from=builder-oneapi2026 /dist/lib/libvmaf.so /usr/local/lib/
ENV VMAFX_BACKEND=sycl

FROM scratch AS node-sycl
"""
        dummy_gpu = "RUN bash scripts/ci/build-oneapi-libvmaf.sh\n"
        errors = validate_node_sycl_contract(bad_node, dummy_gpu)
        self.assertTrue(
            any("does not invoke scripts/ci/build-oneapi-libvmaf.sh" in err for err in errors),
            f"Expected missing script invocation error, got: {errors}",
        )

    def test_planted_negative_cpu_licence_scan(self) -> None:
        """Planted negative: the node-sycl notices read go-builder's CPU scan."""
        node_text = DOCKERFILE_NODE.read_text(encoding="utf-8")
        bad_node = node_text.replace(
            "from=go-licences-sycl,source=/licence-inputs-sycl",
            "from=go-builder,source=/licence-inputs",
            1,
        )
        self.assertNotEqual(bad_node, node_text)
        gpu_text = DOCKERFILE_PROD_GPU.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(bad_node, gpu_text)
        self.assertTrue(
            any("node-sycl-notices must bind /licence-inputs" in err for err in errors),
            f"Expected CPU licence scan error, got: {errors}",
        )

    def test_licence_manifest_records_gpu_stack(self) -> None:
        manifest = json.loads(LICENSING_JSON.read_text(encoding="utf-8"))
        self.assertEqual(validate_licence_manifest(manifest), [])

    def test_planted_negative_manifest_without_gpu_stack(self) -> None:
        """Planted negative: the manifest drops intel-gpu-stack from the node-sycl artifact."""
        manifest = json.loads(LICENSING_JSON.read_text(encoding="utf-8"))
        record = manifest["artifacts"]["production-node-sycl-image"]
        record["components"] = [c for c in record["components"] if c.get("id") != "intel-gpu-stack"]
        errors = validate_licence_manifest(manifest)
        self.assertTrue(any("intel-gpu-stack" in err for err in errors), errors)

    def test_planted_negative_missing_gpu_stack_copy(self) -> None:
        """Planted negative: node-sycl ships without the Level Zero loader and GPU driver."""
        node_text = DOCKERFILE_NODE.read_text(encoding="utf-8")
        bad_node = node_text.replace("COPY --from=intel-gpu-runtime-libs /out/ /\n", "", 1)
        self.assertNotEqual(bad_node, node_text)
        gpu_text = DOCKERFILE_PROD_GPU.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(bad_node, gpu_text)
        self.assertTrue(
            any("does not copy the Intel GPU stack" in err for err in errors),
            f"Expected missing GPU stack error, got: {errors}",
        )

    def test_planted_negative_copied_md5sums(self) -> None:
        """Planted negative: md5sums copied from dpkg info, which gmmlib lacks."""
        node_text = DOCKERFILE_NODE.read_text(encoding="utf-8")
        bad_node = node_text.replace(
            '        test -s "/out/var/lib/dpkg/status.d/${pkg}.md5sums" || exit 1; \\\n', "", 1
        )
        self.assertNotEqual(bad_node, node_text)
        gpu_text = DOCKERFILE_PROD_GPU.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(bad_node, gpu_text)
        self.assertTrue(
            any("md5sums list" in err for err in errors),
            f"Expected md5sums error, got: {errors}",
        )

    def test_planted_negative_runtime_check_not_ordered(self) -> None:
        """Planted negative: the final stage no longer depends on the runtime check."""
        node_text = DOCKERFILE_NODE.read_text(encoding="utf-8")
        bad_node = node_text.replace(
            "from=node-sycl-runtime-check", "from=node-sycl-licence-check", 1
        )
        self.assertNotEqual(bad_node, node_text)
        gpu_text = DOCKERFILE_PROD_GPU.read_text(encoding="utf-8")
        errors = validate_node_sycl_contract(bad_node, gpu_text)
        self.assertTrue(
            any("does not order node-sycl-runtime-check" in err for err in errors),
            f"Expected runtime check error, got: {errors}",
        )

    def test_boundary_missing_node_sycl_stages(self) -> None:
        """Boundary test: completely empty or non-node Dockerfile."""
        empty_node = "FROM scratch\n"
        dummy_gpu = "RUN bash scripts/ci/build-oneapi-libvmaf.sh\n"
        errors = validate_node_sycl_contract(empty_node, dummy_gpu)
        self.assertTrue(
            any("missing stage 'builder-oneapi2026'" in err for err in errors),
            f"Expected missing stage error, got: {errors}",
        )


if __name__ == "__main__":
    unittest.main()
