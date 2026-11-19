#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract test for the Windows MSVC+SYCL oneAPI toolchain pin in libvmaf-build-matrix.yml."""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Any

import yaml  # type: ignore[import-untyped]

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW_PATH = REPO_ROOT / ".github" / "workflows" / "libvmaf-build-matrix.yml"

WINDOWS_GPU_JOB_ID = "windows-gpu-build"
ONEAPI_STEP_NAME = "Install Intel oneAPI (windows)"
MSVC_STEP_NAME = "Setup MSVC dev environment"
EXPECTED_MSVC_ACTION = "TheMrMilchmann/setup-msvc-dev@368ef7d1ee4d1171b31d4a7f67f4d954f903f5a9"
EXPECTED_WINDOWS_BASEKIT_URL = (
    "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/"
    "728c4418-1b40-47a9-ad51-31530fb27c94/"
    "intel-oneapi-toolkit-2026.2.0.661_offline.exe"
)
OLD_WINDOWS_BASEKIT_URL = (
    "https://registrationcenter-download.intel.com/akdlm/IRC_NAS/"
    "1f18901e-877d-469d-a41a-a10f11b39336/"
    "intel-oneapi-base-toolkit-2025.3.0.372_offline.exe"
)
EXPECTED_DPCPP_COMPONENT = "intel.oneapi.win.cpp-dpcpp-common"
EXPECTED_SYCL_STEP_IF = "steps.leg.outputs.run == 'true' && matrix.backend == 'sycl'"
BANNED_STL_MISMATCH_MACRO = "_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
MIN_ONEAPI_MAJOR = 2026

MAX_MATRIX_ROWS = 16
MAX_JOB_STEPS = 64
ONEAPI_URL_VERSION_RE = re.compile(
    r"intel-oneapi-(?:base-)?toolkit-(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)\.(?P<build>\d+)_offline\.exe$"
)


def _load_windows_gpu_job(workflow_text: str) -> dict[str, Any]:
    """Parse workflow YAML and return the windows-gpu-build job mapping."""
    if BANNED_STL_MISMATCH_MACRO in workflow_text:
        raise AssertionError(f"workflow must not use unsupported {BANNED_STL_MISMATCH_MACRO} macro")
    parsed = yaml.safe_load(workflow_text)
    if not isinstance(parsed, dict):
        raise AssertionError("workflow YAML must parse to a mapping")
    jobs = parsed.get("jobs")
    if not isinstance(jobs, dict) or WINDOWS_GPU_JOB_ID not in jobs:
        raise AssertionError(f"workflow missing job '{WINDOWS_GPU_JOB_ID}'")
    job = jobs[WINDOWS_GPU_JOB_ID]
    if not isinstance(job, dict):
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' must be a mapping")
    return job


def _validate_matrix_backends(job: dict[str, Any]) -> None:
    """Verify windows-gpu-build matrix declares both cuda and sycl legs without a cuda-only pin."""
    include = job.get("strategy", {}).get("matrix", {}).get("include")
    if not isinstance(include, list) or not include:
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' missing strategy.matrix.include")
    backends: set[str] = set()
    for row in include[:MAX_MATRIX_ROWS]:
        if not isinstance(row, dict):
            continue
        backend = str(row.get("backend", ""))
        backends.add(backend)
        if backend == "cuda" and (
            "WINDOWS_BASEKIT_URL" in row or "oneapi" in str(row).lower() or "toolset" in row
        ):
            raise AssertionError("CUDA matrix row must not carry the SYCL toolchain pin")
    if backends != {"cuda", "sycl"}:
        raise AssertionError(f"expected matrix backends {{'cuda', 'sycl'}}, found {backends}")


def _validate_step_if_conditions(steps: list[dict[str, Any]]) -> None:
    """Ensure every post-checkout step in windows-gpu-build keeps its explicit if: guard."""
    for idx, step in enumerate(steps[:MAX_JOB_STEPS]):
        uses = str(step.get("uses", ""))
        name = str(step.get("name", f"step[{idx}]"))
        if idx == 0 and step.get("id") == "leg":
            continue
        if uses.startswith("actions/checkout@"):
            continue
        condition = step.get("if")
        if not isinstance(condition, str) or "steps.leg.outputs.run == 'true'" not in condition:
            raise AssertionError(f"step '{name}' must keep its steps.leg.outputs.run if: guard")


def _validate_msvc_step(steps: list[dict[str, Any]]) -> None:
    """Verify the setup-msvc-dev step remains SHA-pinned at v4.1.0 with arch: x64."""
    msvc_steps = [s for s in steps[:MAX_JOB_STEPS] if s.get("name") == MSVC_STEP_NAME]
    if len(msvc_steps) != 1:
        raise AssertionError(f"expected exactly one '{MSVC_STEP_NAME}' step")
    step = msvc_steps[0]
    if step.get("uses") != EXPECTED_MSVC_ACTION:
        raise AssertionError(f"'{MSVC_STEP_NAME}' must stay pinned to {EXPECTED_MSVC_ACTION}")
    with_map = step.get("with")
    if not isinstance(with_map, dict) or with_map.get("arch") != "x64":
        raise AssertionError(f"'{MSVC_STEP_NAME}' must specify with.arch: x64")


def _validate_oneapi_step(steps: list[dict[str, Any]]) -> None:
    """Verify the SYCL leg pins the Clang-22+ oneAPI 2026.2 offline installer."""
    oneapi_steps = [s for s in steps[:MAX_JOB_STEPS] if s.get("name") == ONEAPI_STEP_NAME]
    if len(oneapi_steps) != 1:
        raise AssertionError(f"expected exactly one '{ONEAPI_STEP_NAME}' step")
    step = oneapi_steps[0]
    if step.get("if") != EXPECTED_SYCL_STEP_IF:
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must run on the SYCL leg ({EXPECTED_SYCL_STEP_IF!r}), "
            f"found {step.get('if')!r}"
        )
    env = step.get("env")
    if not isinstance(env, dict):
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' must define an env mapping")
    url = str(env.get("WINDOWS_BASEKIT_URL", ""))
    if not url:
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' missing WINDOWS_BASEKIT_URL pin")
    match = ONEAPI_URL_VERSION_RE.search(url)
    if match is None or int(match.group("major")) < MIN_ONEAPI_MAJOR:
        raise AssertionError(
            f"WINDOWS_BASEKIT_URL must pin oneAPI >= {MIN_ONEAPI_MAJOR} (Clang 22+ for MSVC 14.52 STL1000), "
            f"found {url!r}"
        )
    if url != EXPECTED_WINDOWS_BASEKIT_URL:
        raise AssertionError(
            f"WINDOWS_BASEKIT_URL must equal {EXPECTED_WINDOWS_BASEKIT_URL!r}, found {url!r}"
        )
    if env.get("WINDOWS_DPCPP_COMPONENTS") != EXPECTED_DPCPP_COMPONENT:
        raise AssertionError(f"WINDOWS_DPCPP_COMPONENTS must equal {EXPECTED_DPCPP_COMPONENT!r}")
    run_script = str(step.get("run", ""))
    if "-p=NEED_VS2026_INTEGRATION=0" not in run_script:
        raise AssertionError(
            f"'{ONEAPI_STEP_NAME}' must pass -p=NEED_VS2026_INTEGRATION=0 on VS 2026 runners"
        )
    if "exit /b %installer_exit_code%" not in run_script:
        raise AssertionError(f"'{ONEAPI_STEP_NAME}' must propagate %installer_exit_code%")


def validate_windows_sycl_toolset_contract(workflow_text: str) -> None:
    """Validate the Windows MSVC+SYCL toolchain pin and step guards in libvmaf-build-matrix.yml."""
    job = _load_windows_gpu_job(workflow_text)
    _validate_matrix_backends(job)
    raw_steps = job.get("steps")
    if not isinstance(raw_steps, list) or not raw_steps:
        raise AssertionError(f"job '{WINDOWS_GPU_JOB_ID}' has no steps")
    steps = [s for s in raw_steps[:MAX_JOB_STEPS] if isinstance(s, dict)]
    _validate_step_if_conditions(steps)
    _validate_msvc_step(steps)
    _validate_oneapi_step(steps)


class WindowsSyclToolsetContractTest(unittest.TestCase):
    """Positive, negative, and boundary contract tests for Windows MSVC+SYCL toolchain pin."""

    def test_live_workflow_satisfies_windows_sycl_toolset_contract(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        validate_windows_sycl_toolset_contract(workflow_text)

    def assert_windows_pin_documentation(self, text: str) -> None:
        self.assertIn("lane pins its own release (2026.2.0.661, ADR-3567)", text)
        self.assertIn("tester zip keeps the build-config.env release (ADR-1566)", text)

    def test_windows_pin_documentation(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            self.assert_windows_pin_documentation((REPO_ROOT / path).read_text(encoding="utf-8"))

    def test_negative_planted_shared_release_rejected(self) -> None:
        for path in ("build-config.env", "docs/development/tester-image.md"):
            text = (REPO_ROOT / path).read_text(encoding="utf-8")
            for marker in (
                "lane pins its own release (2026.2.0.661, ADR-3567)",
                "tester zip keeps the build-config.env release (ADR-1566)",
            ):
                with self.assertRaises(AssertionError):
                    self.assert_windows_pin_documentation(text.replace(marker, "shared release"))

    def test_contract_registered_in_tooling_suite(self) -> None:
        sys.path.insert(0, str(REPO_ROOT))
        from scripts.ci.suite_registry import load_registry, suite_files  # noqa: PLC0415

        rel_path = Path(__file__).resolve().relative_to(REPO_ROOT).as_posix()
        registry = load_registry(REPO_ROOT)
        self.assertEqual(suite_files(registry, [rel_path], "tooling"), [rel_path])

    def test_negative_planted_old_2025_3_0_pin_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(
            EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL
        ).replace(" -p=NEED_VS2026_INTEGRATION=0", "")
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("oneAPI >= 2026", str(ctx.exception))

    def test_negative_planted_missing_url_pin_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = re.sub(r"(?m)^\s*WINDOWS_BASEKIT_URL:.*\n", "", workflow_text)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("missing WINDOWS_BASEKIT_URL pin", str(ctx.exception))

    def test_negative_planted_missing_vs2026_integration_skip_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(" -p=NEED_VS2026_INTEGRATION=0", "", 1)
        self.assertNotEqual(workflow_text, planted)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("-p=NEED_VS2026_INTEGRATION=0", str(ctx.exception))

    def test_negative_planted_stl_mismatch_macro_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        planted = workflow_text.replace(
            "-Dcpp_std=c++latest",
            f"-Dcpp_std=c++latest -D{BANNED_STL_MISMATCH_MACRO}",
            1,
        )
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn(BANNED_STL_MISMATCH_MACRO, str(ctx.exception))

    def test_negative_planted_missing_step_if_guard_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        target = f"      - name: {ONEAPI_STEP_NAME}\n" f"        if: {EXPECTED_SYCL_STEP_IF}\n"
        replacement = f"      - name: {ONEAPI_STEP_NAME}\n"
        planted = workflow_text.replace(target, replacement, 1)
        self.assertNotEqual(workflow_text, planted)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted)
        self.assertIn("if: guard", str(ctx.exception))

    def test_boundary_pin_present_on_cuda_leg_only_rejected(self) -> None:
        workflow_text = WORKFLOW_PATH.read_text(encoding="utf-8")
        target = f"      - name: {ONEAPI_STEP_NAME}\n" f"        if: {EXPECTED_SYCL_STEP_IF}\n"
        cuda_only_step = (
            f"      - name: {ONEAPI_STEP_NAME}\n"
            "        if: steps.leg.outputs.run == 'true' && matrix.backend == 'cuda'\n"
        )
        planted_step = workflow_text.replace(target, cuda_only_step, 1)
        self.assertNotEqual(workflow_text, planted_step)
        with self.assertRaises(AssertionError) as ctx:
            validate_windows_sycl_toolset_contract(planted_step)
        self.assertIn("must run on the SYCL leg", str(ctx.exception))

        cuda_matrix_pin = workflow_text.replace(
            "          - backend: cuda\n",
            f"          - backend: cuda\n            WINDOWS_BASEKIT_URL: {EXPECTED_WINDOWS_BASEKIT_URL}\n",
            1,
        ).replace(EXPECTED_WINDOWS_BASEKIT_URL, OLD_WINDOWS_BASEKIT_URL)
        with self.assertRaises(AssertionError) as ctx2:
            validate_windows_sycl_toolset_contract(cuda_matrix_pin)
        self.assertIn("CUDA matrix row", str(ctx2.exception))


if __name__ == "__main__":
    unittest.main()
