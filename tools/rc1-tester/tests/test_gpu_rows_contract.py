# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The GPU images' row maps stay in step with the ledger, the tests and the gate.

`tools/rc1-tester/image/<backend>-rows.json` names, per state row of docs/state.md
and device family, the device tests, audits and gate cells that close it. This
keeps each map honest without a device: every row exists in docs/state.md and
every test it names is spelled out in that row, every test is a Meson test of the
`gpu` suite, every family is one the backend's probe knows, every audit is one
the backend parses, and every gate feature is a feature of the parity gate.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "rc1-tester" / "src"))
sys.path.insert(0, str(ROOT))

from scripts.ci.cross_backend_parity_gate import FEATURE_METRICS

from vmaf_rc1_tester import hw_cudaprobe, hw_hipprobe, hw_l0probe
from vmaf_rc1_tester.hw_cuda import CUDA
from vmaf_rc1_tester.hw_hip import HIP
from vmaf_rc1_tester.hw_sycl import SYCL

STATE = (ROOT / "docs" / "state.md").read_text(encoding="utf-8")
MESON = (ROOT / "core" / "test" / "meson.build").read_text(encoding="utf-8")
IMAGE = ROOT / "tools" / "rc1-tester" / "image"
BACKENDS = {
    "sycl": (SYCL, {family for *_, family in hw_l0probe.FAMILIES}),
    "cuda": (CUDA, set(hw_cudaprobe.FAMILY_NAMES)),
    "hip": (HIP, set(hw_hipprobe.FAMILY_NAMES)),
}


def rows_of(backend: str) -> list[dict]:
    document = json.loads((IMAGE / f"{backend}-rows.json").read_text(encoding="utf-8"))
    assert document["backend"] == backend
    return document["rows"]


def state_row(row_id: str) -> str:
    match = re.search(r"^\| \*\*" + re.escape(row_id) + r"\*\* \|.*$", STATE, re.MULTILINE)
    assert match, f"{row_id} is not a row of docs/state.md"
    return match.group(0)


def meson_suites(test: str) -> str:
    match = re.search(r"test\('" + re.escape(test) + r"',.*?\)\n", MESON, re.DOTALL)
    assert match, f"{test} is not a Meson test of core/test/meson.build"
    return match.group(0)


@pytest.mark.parametrize("backend", sorted(BACKENDS))
def test_rows_exist_and_spell_out_their_tests(backend: str) -> None:
    for row in rows_of(backend):
        text = state_row(row["id"])
        for test in row["tests"]:
            assert test in text, f"{row['id']} does not name {test}"


@pytest.mark.parametrize("backend", sorted(BACKENDS))
def test_named_tests_are_gpu_suite_tests(backend: str) -> None:
    for row in rows_of(backend):
        for test in row["tests"] + row.get("audits", []):
            assert "'gpu'" in meson_suites(test), f"{test} is not in the gpu suite"


@pytest.mark.parametrize("backend", sorted(BACKENDS))
def test_families_audits_and_gate_features_are_known(backend: str) -> None:
    gpu_backend, families = BACKENDS[backend]
    for row in rows_of(backend):
        assert set(row["families"]) <= families
        assert set(row.get("audits", [])) <= set(gpu_backend.audits)
        assert {spec["feature"] for spec in row.get("gate", [])} <= set(FEATURE_METRICS)


def test_every_family_of_the_default_aot_list_has_a_sycl_row() -> None:
    covered = {family for row in rows_of("sycl") for family in row["families"]}
    assert {"xe-lp", "xe-lpg", "xe-hpg", "xe2"} <= covered


def test_every_cuda_family_has_a_row_holding_every_gate_feature() -> None:
    rows = rows_of("cuda")
    assert {family for row in rows for family in row["families"]} == set(hw_cudaprobe.FAMILY_NAMES)
    for row in rows:  # a new gate feature changes the row map, not only the gate
        assert {spec["feature"] for spec in row["gate"]} == set(FEATURE_METRICS)


def test_every_family_the_hip_build_targets_has_a_row_holding_every_gate_feature() -> None:
    rows = rows_of("hip")
    covered = {family for row in rows for family in row["families"]}
    dockerfile = (ROOT / "docker" / "Dockerfile.tester").read_text(encoding="utf-8")
    targets = (
        re.search(r"^ARG HIP_GFX_TARGETS=(\S+)$", dockerfile, re.MULTILINE).group(1).split(",")
    )
    assert {hw_hipprobe.family_of(target) for target in targets} <= covered
    for row in rows:
        assert {spec["feature"] for spec in row["gate"]} == set(FEATURE_METRICS)
