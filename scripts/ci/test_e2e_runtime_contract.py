#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Guard the Kubernetes E2E workflow's executable runtime contract."""

from __future__ import annotations

import json
import re
import unittest
from collections.abc import Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "e2e-k8s.yml"
RULES_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "rule-enforcement.yml"
NODE_DOCKERFILE = REPO_ROOT / "docker" / "Dockerfile.node"
SERVER_DOCKERFILE = REPO_ROOT / "Dockerfile.go-server"
ROOT_DOCKERFILE = REPO_ROOT / "Dockerfile"
FFMPEG_DOCKERFILE = REPO_ROOT / "Dockerfile.ffmpeg"
DEV_CONTAINERFILE = REPO_ROOT / "dev" / "Containerfile"
FFMPEG_SERIES = REPO_ROOT / "ffmpeg-patches" / "series.txt"
CORE_MESON = REPO_ROOT / "core" / "meson.build"
MSVC_CLZ_GUARD = "scripts/ci/check-msvc-clz-shim.sh"
GITIGNORE = REPO_ROOT / ".gitignore"
KIND_SCRIPT = REPO_ROOT / "test" / "e2e" / "kind-cluster.sh"
ASSERT_CONTEXT_SCRIPT = REPO_ROOT / "test" / "e2e" / "assert-kind-context.sh"
KUTTL_CASES = REPO_ROOT / "test" / "e2e" / "kuttl-tests"
KUTTL_CONFIG = KUTTL_CASES / "kuttl-test.yaml"
INSTALL_STEP = KUTTL_CASES / "01-chart-cpu-score" / "00-install.yaml"
READY_STEP = KUTTL_CASES / "01-chart-cpu-score" / "01-ready.yaml"
SCORE_STEP = KUTTL_CASES / "01-chart-cpu-score" / "02-score.yaml"
SCORE_SCRIPT = REPO_ROOT / "test" / "e2e" / "score-smoke.sh"
FIXTURE_SCRIPT = REPO_ROOT / "test" / "e2e" / "fixtures" / "gen-tiny-yuv.sh"
DEFAULT_MODEL_HEADER = REPO_ROOT / "core" / "include" / "libvmaf" / "model.h"
CAMBI_HEADER = REPO_ROOT / "core" / "src" / "feature" / "cambi_internal.h"
SPEED_HEADER = REPO_ROOT / "core" / "src" / "feature" / "speed_internal.h"
MODEL_DIR = REPO_ROOT / "model"
# Kubernetes rejects a ConfigMap whose data exceeds 1 MiB.
CONFIGMAP_DATA_LIMIT = 1024 * 1024
CHART_TEMPLATES = REPO_ROOT / "deploy" / "helm" / "vmafx" / "templates"
NODE_BUILD_STEP = "Build vmafx-node image (cpu variant, e2e tag)"
SERVER_BUILD_STEP = "Build vmafx-server image (cpu variant, e2e tag)"
IMAGE_NAMES = (
    "ghcr.io/vmafx/vmafx-operator:e2e-test",
    "ghcr.io/vmafx/vmafx-node:e2e-test",
    "ghcr.io/vmafx/vmafx-server:e2e-test",
)


def run_blocks(workflow: str) -> list[str]:
    """Every `run:` block in the workflow, to the end of its indented body."""
    blocks = []
    lines = workflow.splitlines()
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith(("run:", "- run:")):
            continue
        indent = len(line) - len(line.lstrip())
        body = [line]
        for following in lines[index + 1 :]:
            if following.strip() and (len(following) - len(following.lstrip())) <= indent:
                break
            body.append(following)
        blocks.append("\n".join(body))
    return blocks


def _workflow_step(workflow: str, name: str) -> str:
    """Return one top-level step block from the E2E workflow."""
    marker = f"      - name: {name}\n"
    start = workflow.find(marker)
    if start < 0:
        raise AssertionError(f"workflow step not found: {name}")

    end = workflow.find("\n      - name:", start + len(marker))
    if end < 0:
        end = len(workflow)
    return workflow[start:end]


def _docker_stage(dockerfile: str, name: str) -> str:
    """Return one named Docker build stage through the next FROM line."""
    marker = f" AS {name}"
    lines = dockerfile.splitlines()
    start = next(
        index
        for index, line in enumerate(lines)
        if line.startswith("FROM ") and line.rstrip().endswith(marker)
    )
    end = next(
        (index for index in range(start + 1, len(lines)) if lines[index].startswith("FROM ")),
        len(lines),
    )
    return "\n".join(lines[start:end])


def _c_define(header: Path, name: str) -> str:
    """Return the replacement text of one `#define` in a C header."""
    source = header.read_text(encoding="utf-8")
    match = re.search(rf"(?m)^#define\s+{name}\s+(.+?)\s*$", source)
    if match is None:
        raise AssertionError(f"{header.name} does not define {name}")
    return match.group(1)


def _c_int_define(header: Path, name: str) -> int:
    """Return an integer `#define`, allowing one pair of parentheses."""
    return int(_c_define(header, name).strip("()"))


def _dimension_thresholds() -> tuple[int, int]:
    """CAMBI's one-axis minimum and SpEED's per-plane minimum, from the C headers."""
    speed_formula = _c_define(SPEED_HEADER, "SPEED_INTERNAL_MIN_DIMENSION")
    if speed_formula != "(SPEED_INTERNAL_BLOCK_SIZE << SPEED_INTERNAL_NUM_SCALES)":
        raise AssertionError(f"SPEED_INTERNAL_MIN_DIMENSION changed form: {speed_formula}")
    speed_min = _c_int_define(SPEED_HEADER, "SPEED_INTERNAL_BLOCK_SIZE") << _c_int_define(
        SPEED_HEADER, "SPEED_INTERNAL_NUM_SCALES"
    )
    return _c_int_define(CAMBI_HEADER, "CAMBI_MIN_WIDTH_HEIGHT"), speed_min


def _default_model_features() -> list[str]:
    """Feature names of the model libvmaf scores with when none is named."""
    version = _c_define(DEFAULT_MODEL_HEADER, "VMAF_DEFAULT_MODEL_VERSION").strip('"')
    candidates = sorted(MODEL_DIR.rglob(f"{version}.json"))
    if len(candidates) != 1:
        raise AssertionError(f"expected one model file for {version}, found {candidates}")
    model = json.loads(candidates[0].read_text(encoding="utf-8"))
    return list(model["model_dict"]["feature_names"])


def yuv420_rejection(features: Sequence[str], width: int, height: int) -> str | None:
    """Why libvmaf would refuse a YUV420P input for a model, or None.

    Mirrors vmaf_validate_model_dimensions() in
    core/src/feature/feature_dimensions.h for the checks that bite 4:2:0
    input, reading the thresholds from the same headers it does.
    """
    cambi_min, speed_min = _dimension_thresholds()
    names = " ".join(features).lower()
    if "cambi" in names and width < cambi_min and height < cambi_min:
        return f"cambi needs width or height >= {cambi_min}; got {width}x{height}"
    chroma_width, chroma_height = width // 2, height // 2
    if "speed_chroma" in names and min(chroma_width, chroma_height) < speed_min:
        return (
            f"speed_chroma needs chroma planes >= {speed_min}; got {chroma_width}x{chroma_height}"
        )
    temporal = "speed_temporal" in names or "speed_qa" in names
    if temporal and min(width, height) < speed_min:
        return f"speed_temporal/speed_qa need width and height >= {speed_min}"
    return None


def _fixture_geometry() -> tuple[int, int, int]:
    """WIDTH, HEIGHT and FRAMES from the fixture generator, their single source."""
    script = FIXTURE_SCRIPT.read_text(encoding="utf-8")
    values = []
    for name in ("WIDTH", "HEIGHT", "FRAMES"):
        match = re.search(rf"(?m)^{name}=([0-9]+)$", script)
        if match is None:
            raise AssertionError(f"{FIXTURE_SCRIPT.name} does not set {name}")
        values.append(int(match.group(1)))
    return values[0], values[1], values[2]


class E2EFixtureGeometryTest(unittest.TestCase):
    """The scoring smoke must send an input the default model can score.

    ADR-1169 made vmaf_v1.0.16_3d0h the default and made undersized input a
    hard error, so a fixture below the model's minimum turns the nightly
    /v1/score into an HTTP 500. The E2E job itself only runs nightly; this
    check runs on every pull request through the Rules workflow.
    """

    # The features of the current default model that carry size limits.
    CONSTRAINED = ("Cambi_feature_cambi_score", "Speed_chroma_feature_speed_chroma_uv_score")

    def test_fixture_geometry_satisfies_default_model(self) -> None:
        width, height, _ = _fixture_geometry()
        self.assertIsNone(yuv420_rejection(_default_model_features(), width, height))

    def test_committed_fixtures_match_the_generator_geometry(self) -> None:
        # A geometry change in gen-tiny-yuv.sh without regenerated raw files
        # would otherwise surface only in the nightly 'Prepare YUV fixtures' job.
        width, height, frames = _fixture_geometry()
        expected = width * height * 3 // 2 * frames
        for name in ("ref.yuv", "dist.yuv"):
            with self.subTest(fixture=name):
                path = REPO_ROOT / "test/e2e/fixtures" / name
                self.assertEqual(path.stat().st_size, expected)

    def test_fixture_pair_fits_one_configmap(self) -> None:
        width, height, frames = _fixture_geometry()
        frame_size = width * height * 3 // 2
        # Two Y4M files: a stream header (under 64 bytes) plus a FRAME marker
        # per frame.
        payload = 2 * (64 + frames * (len(b"FRAME\n") + frame_size))
        self.assertLessEqual(payload, CONFIGMAP_DATA_LIMIT)

    def test_rejection_boundaries_match_libvmaf(self) -> None:
        # The same boundaries the vmaf CLI enforces for this feature set:
        # 216x160 scores, 214x160 fails on cambi, 216x158 on speed_chroma.
        cambi_min, speed_min = _dimension_thresholds()
        luma_min = 2 * speed_min
        self.assertIsNone(yuv420_rejection(self.CONSTRAINED, cambi_min, luma_min))
        self.assertIsNone(yuv420_rejection(self.CONSTRAINED, luma_min, cambi_min))
        self.assertIn("cambi", yuv420_rejection(self.CONSTRAINED, cambi_min - 2, luma_min) or "")
        self.assertIn(
            "speed_chroma", yuv420_rejection(self.CONSTRAINED, cambi_min, luma_min - 2) or ""
        )
        self.assertIsNotNone(yuv420_rejection(self.CONSTRAINED, 64, 64))

    def test_unconstrained_model_accepts_small_input(self) -> None:
        features = ["VMAF_integer_feature_adm2_score", "VMAF_integer_feature_motion2_score"]
        self.assertIsNone(yuv420_rejection(features, 64, 64))


class E2ERuntimeContractTest(unittest.TestCase):
    """Keep the nightly test aligned with artifacts it actually executes."""

    # Declared so the type checker knows setUpClass introduces it; assigning in
    # setUpClass alone leaves every use of self.workflow an unknown attribute.
    workflow: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_cpu_node_and_server_targets_are_explicit(self) -> None:
        node_step = _workflow_step(self.workflow, NODE_BUILD_STEP)
        server_step = _workflow_step(self.workflow, SERVER_BUILD_STEP)

        self.assertRegex(node_step, r"(?m)^\s+file:\s+docker/Dockerfile\.node\s*$")
        self.assertRegex(node_step, r"(?m)^\s+target:\s+node-cpu\s*$")
        self.assertNotIn("BACKEND=cpu", node_step)
        self.assertRegex(server_step, r"(?m)^\s+file:\s+Dockerfile\.go-server\s*$")
        self.assertRegex(server_step, r"(?m)^\s+target:\s+go-server\s*$")

        node_dockerfile = NODE_DOCKERFILE.read_text(encoding="utf-8")
        server_dockerfile = SERVER_DOCKERFILE.read_text(encoding="utf-8")
        self.assertRegex(node_dockerfile, r"(?m)^FROM\s+runtime-base\s+AS\s+node-cpu\s*$")
        self.assertIn("cp -r model/. /dist/model/", node_dockerfile)
        self.assertIn("test -f /dist/model/vmaf_v0.6.1.json", node_dockerfile)
        self.assertNotIn("cp -r model/ /dist/model/", node_dockerfile)
        self.assertRegex(server_dockerfile, r"(?m)^FROM\s+.+\s+AS\s+go-server\s*$")

    def test_partial_image_contexts_include_configure_time_source_guards(self) -> None:
        copy = f"COPY {MSVC_CLZ_GUARD} {MSVC_CLZ_GUARD}"

        for dockerfile in (NODE_DOCKERFILE, SERVER_DOCKERFILE):
            with self.subTest(dockerfile=dockerfile.name):
                source = dockerfile.read_text(encoding="utf-8")
                self.assertIn(copy, source)
                self.assertLess(source.index(copy), source.index("RUN meson setup /build core"))

    def test_partial_image_builders_preserve_libvmaf_build_contract(self) -> None:
        for dockerfile in (NODE_DOCKERFILE, SERVER_DOCKERFILE):
            with self.subTest(dockerfile=dockerfile.name):
                source = dockerfile.read_text(encoding="utf-8")
                builder = _docker_stage(source, "vmaf-builder")
                self.assertRegex(builder, r"(?m)^\s+make \\\s*$")
                self.assertRegex(builder, r"(?m)^\s+xxd \\\s*$")
                self.assertIn("cp -a /build/src/libvmaf.so* /dist/lib/", builder)
                self.assertIn(
                    "cp /build/meson-private/libvmaf.pc /dist/lib/pkgconfig/libvmaf.pc",
                    builder,
                )
                self.assertNotIn("find /build/src", builder)

        node = NODE_DOCKERFILE.read_text(encoding="utf-8")
        self.assertNotIn('pkg_version="${VMAFX_VERSION#v}"', node)

    def test_ffmpeg_builders_fail_on_every_diagnostic(self) -> None:
        builders = (
            ROOT_DOCKERFILE,
            FFMPEG_DOCKERFILE,
            DEV_CONTAINERFILE,
            NODE_DOCKERFILE,
        )

        for dockerfile in builders:
            with self.subTest(dockerfile=dockerfile.name):
                source = dockerfile.read_text(encoding="utf-8")
                self.assertIn("--fatal-warnings", source)
                self.assertIn("tee /tmp/ffmpeg-build.log", source)
                self.assertIn("grep -Ei '(^|[[:space:]])warning([[:space:]#:])'", source)
                # The message differs by builder because the scope does. What is
                # pinned is that a matched warning aborts the build, not one
                # exact sentence.
                self.assertRegex(source, r"FATAL: .*compiler warnings")

        # These builders compile far more of upstream FFmpeg than the CI
        # workflow legs do -- gpl, nonfree and some twenty external libraries,
        # against whatever GCC each base image ships -- so their gate covers the
        # files this fork patches rather than all of upstream. Upstream's own
        # diagnostics under one distro's compiler are not a line this fork can
        # hold. The narrowing is only safe while the file list is derived and
        # non-empty: an empty list would turn the grep into a gate that always
        # passes. Both halves are pinned so the scoping cannot decay into a
        # vacuous check.
        for dockerfile in (ROOT_DOCKERFILE, NODE_DOCKERFILE, DEV_CONTAINERFILE):
            with self.subTest(scoped=dockerfile.name):
                scoped = dockerfile.read_text(encoding="utf-8")
                self.assertIn("ffmpeg-patches/*.patch", scoped)
                self.assertIn("grep -Ff /tmp/ffmpeg-patched-files.txt", scoped)
                self.assertIn("if [ ! -s /tmp/ffmpeg-patched-files.txt ]", scoped)
                self.assertIn("derived no patched-file list", scoped)

        compatibility = FFMPEG_DOCKERFILE.read_text(encoding="utf-8")
        self.assertIn("COPY ffmpeg-patches/ /tmp/ffmpeg-patches/", compatibility)
        self.assertIn("done < /tmp/ffmpeg-patches/series.txt", compatibility)
        self.assertNotIn("ffmpeg-libvmaf-gpu.patch", compatibility)
        self.assertNotIn("--enable-libnpp", compatibility)

        dev = DEV_CONTAINERFILE.read_text(encoding="utf-8")
        self.assertIn("FATAL %s missing from ffmpeg -encoders", dev)
        self.assertNotIn("WARN %s missing from ffmpeg -encoders", dev)
        self.assertIn('test "$missing" -eq 0', dev)

        for dockerfile in (ROOT_DOCKERFILE, FFMPEG_DOCKERFILE):
            with self.subTest(patch_replay=dockerfile.name):
                source = dockerfile.read_text(encoding="utf-8")
                self.assertIn("FATAL: patch", source)
                self.assertNotIn("patch -p1 <", source)

        series = FFMPEG_SERIES.read_text(encoding="utf-8").splitlines()
        self.assertIn("0019-ffmpeg-eliminate-gcc-14-build-diagnostics.patch", series)

    def test_language_standards_use_warning_clean_meson_options(self) -> None:
        """Language standards come from Meson's built-in preference lists.

        This asserts ADR-1056, which is the policy the tree implements.
        ADR-1273 would replace it with `c_std=c23,c2x,c17` / `cpp_std=c++23,
        c++latest` and no `/std:` injection at all, but it is still **Proposed**
        -- so asserting its spelling here would make the contract test enforce a
        decision that has not been taken. Two parts of ADR-1056 are load-bearing
        and are pinned individually below rather than by matching one long string.
        """
        meson = CORE_MESON.read_text(encoding="utf-8")

        # The trailing `none` is not cosmetic: Meson's intel-llvm-cl backend
        # advertises only c89/c99/c11, so it is the sole entry that backend can
        # accept and configure aborts on the Windows MSVC+SYCL leg without it.
        self.assertIn("'c_std=c23,c2x,c17,none'", meson)
        self.assertIn("'cpp_std=c++26,c++23,c++latest'", meson)

        # Accepting a C++23 spelling does not prove the paired standard library
        # ships the API, so the probe stays regardless of which ADR governs.
        self.assertIn("std::expected in selected C++ standard", meson)

        # No hand-rolled standard plumbing: those bypass Meson's compiler checks.
        self.assertNotIn("c_std_args", meson)
        self.assertNotIn("cxx_std_flag", meson)

        # `/std:clatest` is injected for MSVC-syntax C drivers only, because Meson
        # validates c_std for that backend and then emits no /std flag. Every other
        # spelling of a standard flag through add_project_arguments stays banned.
        std_injections = re.findall(r"add_project_arguments\([^\n]*(?:-std=|/std:)[^\n]*", meson)
        self.assertEqual(
            std_injections,
            ["add_project_arguments('/std:clatest', language: 'c')"],
            "only the MSVC /std:clatest injection is permitted (ADR-1056)",
        )

    def test_all_runtime_images_are_exported_and_loaded(self) -> None:
        export_step = _workflow_step(self.workflow, "Export images as tar for transfer to e2e job")
        load_step = _workflow_step(self.workflow, "Load e2e images into kind cluster")

        for image in IMAGE_NAMES:
            with self.subTest(image=image):
                self.assertIn(image, export_step)
                self.assertIn(image, load_step)

    def test_chart_smoke_uses_exact_local_images_and_cpu(self) -> None:
        install = INSTALL_STEP.read_text(encoding="utf-8")
        ready = READY_STEP.read_text(encoding="utf-8")
        score = SCORE_STEP.read_text(encoding="utf-8")

        self.assertIn("helm upgrade --install vmafx", install)
        self.assertIn("--set image.tag=e2e-test", install)
        self.assertIn("--set image.pullPolicy=Never", install)
        self.assertIn("--set operator.image.tag=e2e-test", install)
        self.assertIn("--set operator.image.pullPolicy=Never", install)
        self.assertIn("--set gpu.vendor=cpu", install)
        self.assertIn("vmafx-e2e-fixtures", install)
        # Client-side apply would copy the fixture payload into an annotation
        # capped at 256 KiB.
        self.assertIn("kubectl apply --server-side", install)
        self.assertIn("--type=strategic", install)
        self.assertIn("kubectl wait --for=condition=Established", ready)
        self.assertIn("kubectl wait --for=condition=Available", ready)
        self.assertFalse((READY_STEP.parent / "01-assert.yaml").exists())
        self.assertIn("bash ../../score-smoke.sh", score)

        score_script = SCORE_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("isinstance(score, bool)", score_script)
        self.assertIn("isinstance(feature_vmaf, bool)", score_script)

    def test_chart_service_selects_only_server_workloads(self) -> None:
        service = (CHART_TEMPLATES / "service.yaml").read_text(encoding="utf-8")
        deployment = (CHART_TEMPLATES / "deployment.yaml").read_text(encoding="utf-8")
        statefulset = (CHART_TEMPLATES / "statefulset.yaml").read_text(encoding="utf-8")
        job = (CHART_TEMPLATES / "job.yaml").read_text(encoding="utf-8")

        self.assertGreaterEqual(service.count("app.kubernetes.io/component: server"), 2)
        # Metadata, Pod template and, since ADR-1353, the workload selector.
        # scripts/ci/check-helm-selector-isolation.py checks the rendered result.
        self.assertGreaterEqual(deployment.count("app.kubernetes.io/component: server"), 3)
        self.assertGreaterEqual(statefulset.count("app.kubernetes.io/component: server"), 5)
        self.assertGreaterEqual(job.count("app.kubernetes.io/component: server"), 2)

    def test_impossible_legacy_scenarios_are_absent(self) -> None:
        legacy_cases = (
            "02-vmafxjob-creates-pod",
            "03-node-heartbeat",
            "04-rclone-score",
            "05-sidecar-trainer",
        )

        for case in legacy_cases:
            with self.subTest(case=case):
                self.assertFalse((KUTTL_CASES / case).exists())

        kind_script = KIND_SCRIPT.read_text(encoding="utf-8")
        self.assertNotIn("helm upgrade --install vmafx-crds", kind_script)
        self.assertNotIn("cert-manager/cert-manager/releases", kind_script)
        self.assertNotIn("fake-device-plugin", kind_script)
        self.assertIn(
            'kubectl --kubeconfig "${KUBECONFIG_PATH}" apply --server-side',
            kind_script,
        )
        self.assertIn('-f "${REPO_ROOT}/deploy/helm/vmafx/crds/"', kind_script)

    def test_contract_runs_in_always_on_rules_workflow(self) -> None:
        rules = RULES_WORKFLOW.read_text(encoding="utf-8")
        invocation = "python3 scripts/ci/test_e2e_runtime_contract.py"

        self.assertIn(invocation, rules)
        self.assertIn(invocation, self.workflow)

    def test_tools_are_runner_local_and_results_need_no_write_token(self) -> None:
        for tool in ("kind", "kubectl", "kuttl"):
            with self.subTest(tool=tool):
                name = f"Install {tool} ${{{{ env.{tool.upper()}_VERSION }}}}"
                step = _workflow_step(self.workflow, name)
                self.assertIn("${RUNNER_TEMP}/vmafx-e2e-tools/bin", step)
                self.assertIn("--retry 5 --retry-all-errors", step)
                self.assertIn('>> "${GITHUB_PATH}"', step)
                self.assertNotIn("/usr/local/bin", step)

        publish = _workflow_step(self.workflow, "Publish test results")
        self.assertIn("hashFiles('test/e2e/results/**/*.xml') != ''", publish)
        self.assertRegex(publish, r"(?m)^\s+check_run:\s+false\s*$")

    def test_kubernetes_mutations_use_an_isolated_kind_context(self) -> None:
        kind_script = KIND_SCRIPT.read_text(encoding="utf-8")
        context_guard = ASSERT_CONTEXT_SCRIPT.read_text(encoding="utf-8")
        kuttl_config = KUTTL_CONFIG.read_text(encoding="utf-8")
        gitignore = GITIGNORE.read_text(encoding="utf-8")

        self.assertIn('kubeconfig_dir="${RUNNER_TEMP}/vmafx-e2e"', self.workflow)
        self.assertIn("printf 'VMAFX_E2E_KUBECONFIG=%s", self.workflow)
        self.assertIn('>> "${GITHUB_ENV}"', self.workflow)
        # A `run:` block has a shell, so it must use ${RUNNER_TEMP}: the
        # kubeconfig and tool-install logic below is written against it, and an
        # expression there would be substituted before the shell ever sees the
        # value. An action input has no shell, and GitHub does not expand
        # environment variables inside `with:`, so ${{ runner.temp }} is the
        # only way to place a cache or artifact path outside the workspace.
        # That is why this is scoped to run blocks rather than the whole file.
        for block in run_blocks(self.workflow):
            self.assertNotIn("${{ runner.temp }}", block)
        self.assertGreaterEqual(self.workflow.count("bash test/e2e/assert-kind-context.sh"), 2)
        self.assertIn("--config test/e2e/kuttl-tests/kuttl-test.yaml", self.workflow)
        self.assertNotIn("kind-cluster.sh --teardown || true", self.workflow)
        self.assertIn('KUBECONFIG_PATH="${VMAFX_E2E_KUBECONFIG:-}"', kind_script)
        self.assertIn('kind create cluster --name "${CLUSTER_NAME}"', kind_script)
        self.assertGreaterEqual(kind_script.count('"${ASSERT_CONTEXT}"'), 3)
        self.assertIn('kubectl --kubeconfig "${KUBECONFIG_PATH}" apply', kind_script)
        self.assertIn('kind delete cluster --name "${CLUSTER_NAME}"', kind_script)
        self.assertIn('EXPECTED_CONTEXT="kind-${CLUSTER_NAME}"', context_guard)
        self.assertIn(r"^https://127\.0\.0\.1:[0-9]+$", context_guard)
        self.assertIn("the process-wide default kubeconfig", context_guard)
        self.assertIn("KUBECONFIG must equal VMAFX_E2E_KUBECONFIG", context_guard)
        self.assertRegex(gitignore, r"(?m)^/kubeconfig$")
        self.assertRegex(kuttl_config, r"(?m)^skipDelete:\s+true\s*$")


if __name__ == "__main__":
    unittest.main()
