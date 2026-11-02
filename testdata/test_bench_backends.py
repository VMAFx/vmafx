#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Unit tests for the per-backend performance baseline harness."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[1]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from scripts.lib.safe_subprocess import (  # noqa: E402
    CommandOutputLimitExceeded,
    CommandTimedOut,
)

_HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location("bench_backends", _HERE / "bench_backends.py")
assert _SPEC is not None and _SPEC.loader is not None
bench_backends = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(bench_backends)


def test_dry_run_lists_16_models() -> None:
    fixtures = [
        f for f in bench_backends.FIXTURES if f.tag in ("src01_576x324", "sollevante_pq_1080p")
    ]
    seen_models: set[str] = set()

    def fake_build_cmd(vmaf_bin, fx, be, mn, out_path, threads):
        seen_models.add(mn)
        return ["fake-vmaf"]

    with patch.object(bench_backends, "build_cmd", side_effect=fake_build_cmd):
        rc = bench_backends.run_dry_run(
            fixtures,
            bench_backends.NEWEST_MODELS,
            ["cpu"],
            "fake-vmaf",
            1,
        )
        assert rc == 0

    assert len(bench_backends.NEWEST_MODELS) == 16
    assert seen_models == set(bench_backends.NEWEST_MODELS)


def test_hdr_sdr_pairing_refused() -> None:
    sdr_fx = next(f for f in bench_backends.FIXTURES if f.tag == "src01_576x324")
    pq_fx = next(f for f in bench_backends.FIXTURES if f.tag == "sollevante_pq_1080p")
    hdr_model = "hdr_vmaf_v1.1.8_3d0h"
    sdr_model = "vmaf_v1.0.16_3d0h"

    assert not bench_backends.can_pair(sdr_fx, hdr_model)
    assert not bench_backends.can_pair(pq_fx, sdr_model)
    assert bench_backends.can_pair(sdr_fx, sdr_model)
    assert bench_backends.can_pair(pq_fx, hdr_model)

    with pytest.raises(ValueError, match="Cannot pair"):
        bench_backends.build_cmd("bin", sdr_fx, "cpu", hdr_model, "/tmp/out.json", 1)

    with pytest.raises(ValueError, match="Cannot pair"):
        bench_backends.build_cmd("bin", pq_fx, "cpu", sdr_model, "/tmp/out.json", 1)


def test_planted_backend_used_mismatch_fallback() -> None:
    doc_cpu_fallback = {
        "backend_used": "cpu",
        "feature_backends": [
            {"extractor": "adm", "backend": "cpu"},
            {"extractor": "motion", "backend": "cpu"},
        ],
    }
    engaged, reason = bench_backends.check_engagement(doc_cpu_fallback, "cuda")
    assert not engaged
    assert "backend_used (cpu) != requested (cuda)" in (reason or "")

    doc_partial_device = {
        "backend_used": "cuda",
        "feature_backends": [
            {"extractor": "adm", "backend": "cuda"},
            {"extractor": "motion", "backend": "cpu"},
        ],
    }
    engaged, reason = bench_backends.check_engagement(doc_partial_device, "cuda")
    assert not engaged
    assert "motion=cpu" in (reason or "")

    doc_device_ok = {
        "backend_used": "cuda",
        "feature_backends": [
            {"extractor": "adm", "backend": "cuda"},
            {"extractor": "motion", "backend": "cuda"},
        ],
    }
    engaged, reason = bench_backends.check_engagement(doc_device_ok, "cuda")
    assert engaged
    assert reason is None


def test_resolve_executable_makes_the_default_bin_absolute() -> None:
    # run_command refuses relative paths with a directory part; the default
    # --vmaf-bin is one, relative to the repository root.
    resolved = bench_backends.resolve_executable("core/build/tools/vmaf")
    assert Path(resolved).is_absolute()
    assert resolved == str(_HERE.parent / "core/build/tools/vmaf")
    assert bench_backends.resolve_executable("vmaf") == "vmaf"
    assert bench_backends.resolve_executable("/opt/vmaf") == "/opt/vmaf"


def test_run_cell_iterations_handles_safe_subprocess_exceptions() -> None:
    for exc in (
        CommandTimedOut(("fake-vmaf",), 5.0, "out", "err"),
        CommandOutputLimitExceeded(("fake-vmaf",), 1024, "out", "err"),
    ):
        with patch.object(bench_backends, "execute_run", side_effect=exc):
            times, first_doc, status = bench_backends.run_cell_iterations(
                ["fake-vmaf"], "unused.json", 1, False
            )
        assert times == []
        assert first_doc is None
        assert status == {
            "status": "unavailable",
            "returncode": None,
            "error": f"{type(exc).__name__}: {exc}",
        }
