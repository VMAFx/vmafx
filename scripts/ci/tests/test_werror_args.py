# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Warnings are errors per leg (ADR-2170): the helper script and the legs that call it."""

from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ci/werror-args.sh"
MATRIX = ROOT / ".github/workflows/libvmaf-build-matrix.yml"
BUILD = ROOT / ".github/workflows/build.yml"
CI_DOC = ROOT / "docs/development/ci.md"
WORKFLOWS = ROOT / ".github/workflows"

# The legs that stay ungated, with the reason ci.md gives. A row not named here must carry
# `werror: true`, so a new leg cannot be added without deciding.
UNGATED_ROWS: set[str] = set()

# The Windows legs that build with cl.exe and link.exe (icx-cl for SYCL). Every one not named
# here is gated with `werror-args.sh msvc`; a name here must be listed in ci.md with its count.
MSVC_LEGS = {
    "Windows MSVC+CUDA",
    "Windows MSVC+SYCL",
    "Windows ARM64 MSVC",
    "Windows MSVC+CUDA (full)",
}
UNGATED_MSVC_LEGS = {"Windows MSVC+SYCL"}
ARGS_OUTPUT = "${{ steps.werror.outputs.args }}"


def run_script(*args: str, os_name: str | None = None) -> subprocess.CompletedProcess[str]:
    env = {key: value for key, value in os.environ.items() if key != "WERROR_ARGS_OS"}
    if os_name is not None:
        env["WERROR_ARGS_OS"] = os_name
    return subprocess.run(  # noqa: S603 -- the script path is this repository's own.
        [str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )


def matrix_rows(text: str) -> dict[str, bool]:
    """Name -> carries `werror: true`, for every row of the build matrix."""
    rows: dict[str, bool] = {}
    for block in re.split(r"\n\s+- os: ", text)[1:]:
        name = re.search(r"^\s+name: (.+)$", block, re.M)
        if name and "CC:" in block:
            rows[name.group(1).strip()] = bool(re.search(r"^\s+werror: true$", block, re.M))
    return rows


def job_block(text: str, job_id: str) -> str:
    """The lines of one top-level job of a workflow, up to the next job."""
    found = re.search(rf"^  {re.escape(job_id)}:\n(.*?)(?=^  [\w-]+:\n|\Z)", text, re.M | re.S)
    return found.group(1) if found else ""


def steps(block: str) -> list[str]:
    return re.split(r"\n      - ", block)[1:]


def wiring_failures(job: str, block: str, call: str) -> list[str]:
    """A gated MSVC job runs the helper in a bash step and hands its output to every cmd
    `meson setup` of the job."""
    failures: list[str] = []
    helper = [step for step in steps(block) if "id: werror" in step]
    if len(helper) != 1 or "shell: bash" not in helper[0] or call not in helper[0]:
        failures.append(f"{job}: no `id: werror` bash step that runs {call}")
    for step in steps(block):
        if "meson setup" in step and "shell: cmd" in step and ARGS_OUTPUT not in step:
            name = step.splitlines()[0]
            failures.append(f"{job}: `{name}` does not pass {ARGS_OUTPUT}")
    return failures


def msvc_legs(matrix: str, build: str) -> tuple[dict[str, bool], list[str]]:
    """Name -> gated for every MSVC leg, and the wiring failures of the gated jobs."""
    legs: dict[str, bool] = {}
    failures: list[str] = []
    matrix_call = 'werror-args.sh "${{ matrix.werror }}")"'
    gpu = job_block(matrix, "windows-gpu-build")
    for row in re.split(r"\n\s+- backend: ", gpu)[1:]:
        name = re.search(r"^\s+name: (.+)$", row, re.M)
        if name:
            legs[name.group(1).strip()] = bool(re.search(r"^\s+werror: msvc$", row, re.M))
    if any(legs.values()):
        failures += wiring_failures("windows-gpu-build", gpu, matrix_call)
    arm = job_block(matrix, "windows-arm64")
    legs["Windows ARM64 MSVC"] = "werror-args.sh msvc)" in arm
    if legs["Windows ARM64 MSVC"]:
        failures += wiring_failures("windows-arm64", arm, "werror-args.sh msvc)")
    work = job_block(build, "build-work")
    for row in re.split(r"\n\s+- os: ", work)[1:]:
        name = re.search(r"^\s+check_name: (.+)$", row, re.M)
        if name and re.search(r"^\s+windows: true$", row, re.M):
            gated = bool(re.search(r"^\s+werror: msvc$", row, re.M))
            legs[name.group(1).strip()] = gated
            if gated:
                failures += wiring_failures("build-work", work, matrix_call)
    return legs, failures


def msvc_contract_failures(matrix: str, build: str, doc: str) -> list[str]:
    legs, failures = msvc_legs(matrix, build)
    if set(legs) != MSVC_LEGS:
        failures.append(f"MSVC legs found {sorted(legs)}, expected {sorted(MSVC_LEGS)}")
    for name, gated in sorted(legs.items()):
        if gated and name in UNGATED_MSVC_LEGS:
            failures.append(f"{name} is ungated in this test but runs werror-args.sh msvc")
        if not gated and name not in UNGATED_MSVC_LEGS:
            failures.append(f"{name} is not gated and not in UNGATED_MSVC_LEGS")
        if name in UNGATED_MSVC_LEGS and name not in doc:
            failures.append(f"{name} is ungated but docs/development/ci.md does not list it")
    return failures


def contract_failures(matrix: str, doc: str) -> list[str]:
    failures: list[str] = []
    rows = matrix_rows(matrix)
    if not rows:
        return ["no build-matrix rows found"]
    for name, gated in sorted(rows.items()):
        if gated and name in UNGATED_ROWS:
            failures.append(f"{name} is ungated in this test but carries werror: true")
        if not gated and name not in UNGATED_ROWS:
            failures.append(f"{name} has no werror: true and is not in UNGATED_ROWS")
        if name in UNGATED_ROWS and name not in doc:
            failures.append(f"{name} is ungated but docs/development/ci.md does not list it")
    if 'werror-args.sh "${{ matrix.werror }}"' not in matrix:
        failures.append("the matrix configure step does not call werror-args.sh")
    return failures


class WerrorArgsScriptTests(unittest.TestCase):
    def test_true_prints_werror_and_the_linker_switch_per_os(self) -> None:
        for os_name, fatal in (
            ("Linux", "-Wl,--fatal-warnings"),
            ("MINGW64_NT-10.0", "-Wl,--fatal-warnings"),
            ("Darwin", "-Wl,-fatal_warnings"),
        ):
            with self.subTest(os=os_name):
                result = run_script("true", os_name=os_name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout.split(),
                    ["-Dwerror=true", f"-Dc_link_args={fatal}", f"-Dcpp_link_args={fatal}"],
                )

    def test_msvc_prints_werror_alone_on_every_os(self) -> None:
        # Meson adds link.exe's -WX itself when werror is set; cl.exe takes /WX from it.
        for os_name in ("MINGW64_NT-10.0-26100", "Linux", "Plan9"):
            with self.subTest(os=os_name):
                result = run_script("msvc", os_name=os_name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.split(), ["-Dwerror=true"])

    def test_unset_or_false_prints_nothing(self) -> None:
        for args in ((), ("",), ("false",)):
            with self.subTest(args=args):
                result = run_script(*args, os_name="Linux")
                self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_a_typo_or_an_unknown_os_is_refused(self) -> None:
        for args, os_name in (
            (("True",), "Linux"),
            (("yes",), "Linux"),
            (("MSVC",), "MINGW64_NT-10.0"),
            (("/WX",), "MINGW64_NT-10.0"),
            (("true",), "Plan9"),
        ):
            with self.subTest(args=args, os=os_name):
                result = run_script(*args, os_name=os_name)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertEqual(result.stdout, "")


class WerrorLegContractTests(unittest.TestCase):
    def test_every_matrix_row_is_gated_or_named_ungated(self) -> None:
        failures = contract_failures(
            MATRIX.read_text(encoding="utf-8"), CI_DOC.read_text(encoding="utf-8")
        )
        self.assertEqual(failures, [])

    def test_a_row_that_loses_its_gate_is_detected(self) -> None:
        matrix = MATRIX.read_text(encoding="utf-8")
        planted = matrix.replace(
            "            werror: true\n            name: Ubuntu gcc\n",
            "            name: Ubuntu gcc\n",
            1,
        )
        self.assertNotEqual(planted, matrix)
        failures = contract_failures(planted, CI_DOC.read_text(encoding="utf-8"))
        self.assertTrue(any("Ubuntu gcc has no werror" in item for item in failures), failures)

    def test_a_new_row_without_a_decision_is_detected(self) -> None:
        matrix = MATRIX.read_text(encoding="utf-8")
        planted = matrix.replace(
            "          - os: ubuntu-latest\n            CC: icx\n",
            "          - os: ubuntu-latest\n            CC: gcc\n            name: Ubuntu planted\n"
            "          - os: ubuntu-latest\n            CC: icx\n",
            1,
        )
        self.assertNotEqual(planted, matrix)
        failures = contract_failures(planted, CI_DOC.read_text(encoding="utf-8"))
        self.assertTrue(any("Ubuntu planted" in item for item in failures), failures)

    def test_every_msvc_leg_is_gated_or_named_ungated(self) -> None:
        failures = msvc_contract_failures(
            MATRIX.read_text(encoding="utf-8"),
            BUILD.read_text(encoding="utf-8"),
            CI_DOC.read_text(encoding="utf-8"),
        )
        self.assertEqual(failures, [])

    def test_an_msvc_row_that_loses_its_gate_is_detected(self) -> None:
        matrix = MATRIX.read_text(encoding="utf-8")
        build = BUILD.read_text(encoding="utf-8")
        doc = CI_DOC.read_text(encoding="utf-8")
        cases = (
            (
                "matrix",
                "            werror: msvc\n            name: Windows MSVC+CUDA\n",
                "            name: Windows MSVC+CUDA\n",
                "Windows MSVC+CUDA is not gated",
            ),
            (
                "build",
                "            windows: true\n            werror: msvc\n",
                "            windows: true\n",
                "Windows MSVC+CUDA (full) is not gated",
            ),
            (
                "matrix",
                'args="$(bash scripts/ci/werror-args.sh msvc)"',
                'args=""',
                "Windows ARM64 MSVC is not gated",
            ),
        )
        for which, old, new, expected in cases:
            with self.subTest(expected=expected):
                planted_matrix = matrix.replace(old, new, 1) if which == "matrix" else matrix
                planted_build = build.replace(old, new, 1) if which == "build" else build
                self.assertNotEqual((planted_matrix, planted_build), (matrix, build))
                failures = msvc_contract_failures(planted_matrix, planted_build, doc)
                self.assertTrue(any(expected in item for item in failures), failures)

    def test_a_configure_step_that_drops_the_arguments_is_detected(self) -> None:
        matrix = MATRIX.read_text(encoding="utf-8")
        build = BUILD.read_text(encoding="utf-8")
        doc = CI_DOC.read_text(encoding="utf-8")
        for which, text in (("matrix", matrix), ("build", build)):
            with self.subTest(workflow=which):
                planted = text.replace(f" {ARGS_OUTPUT}", "", 1)
                self.assertNotEqual(planted, text)
                failures = msvc_contract_failures(
                    planted if which == "matrix" else matrix,
                    planted if which == "build" else build,
                    doc,
                )
                self.assertTrue(any("does not pass" in item for item in failures), failures)

    def test_every_call_names_the_script_that_exists(self) -> None:
        self.assertTrue(os.access(SCRIPT, os.X_OK), "scripts/ci/werror-args.sh is not executable")
        calls = 0
        for workflow in sorted(WORKFLOWS.glob("*.yml")):
            for match in re.finditer(
                r"\$\((?:bash )?((?:\.\./)?scripts/ci/werror-args\.sh)\b",
                workflow.read_text(encoding="utf-8"),
            ):
                calls += 1
                base = ROOT / "core" if match.group(1).startswith("../") else ROOT
                self.assertTrue(
                    (base / match.group(1)).resolve().is_file(),
                    f"{workflow.name}: {match.group(1)}",
                )
        self.assertGreaterEqual(calls, 6)


if __name__ == "__main__":
    unittest.main()
