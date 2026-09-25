# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for replay manifests on legacy AI extractor utilities."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
from types import ModuleType

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"
EXPECTED_BENCHMARK_CALLS = 9
EXPECTED_FALLBACK_WARNINGS = 6


def _load_script(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, SCRIPTS_DIR / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def test_collect_gpu_calibration_manifest(tmp_path: Path) -> None:
    mod = _load_script("collect_gpu_calibration_data")
    report = tmp_path / "gpu_calibration.manifest.json"
    args = argparse.Namespace(
        vmaf_binary=tmp_path / "vmaf",
        reference=tmp_path / "ref.yuv",
        distorted=tmp_path / "dis.yuv",
        width=16,
        height=16,
        pixel_format="420",
        bitdepth=8,
        arch_id="cuda:smoke",
        cuda_device=0,
        sycl_device=0,
        smoke=True,
        output=tmp_path / "calibration.parquet",
        manifest_out=report,
    )

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--smoke"],
        features=["vif"],
        backends=["cuda"],
        frame_limit=100,
        row_count=4,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "gpu-calibration-data-manifest-v1"
    assert payload["selection"] == {"backends": ["cuda"], "features": ["vif"], "frame_limit": 100}
    assert payload["row_count"] == 4
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_collect_gpu_calibration_help_names_current_default_backend(
    capsys: pytest.CaptureFixture[str],
) -> None:
    mod = _load_script("collect_gpu_calibration_data")

    try:
        mod.parse_args(["--help"])
    except SystemExit as exc:
        assert exc.code == 0
    else:  # pragma: no cover - argparse help always exits
        raise AssertionError("--help must exit through argparse")

    help_text = capsys.readouterr().out
    assert "lavapipe" not in help_text.lower()
    assert "default: cuda only" in help_text


@pytest.mark.parametrize("payload", ({}, [], {"frames": {}}, {"frames": [1]}))
def test_collect_gpu_calibration_rejects_malformed_frames(
    tmp_path: Path,
    payload: object,
) -> None:
    mod = _load_script("collect_gpu_calibration_data")
    report = tmp_path / "malformed.json"
    report.write_text(json.dumps(payload), encoding="utf-8")

    with pytest.raises(ValueError, match=r"expected|every frame"):
        mod.load_frames(report)


def test_benchmark_harness_uses_current_portable_contract() -> None:
    script = REPO_ROOT / "testdata" / "bench_all.sh"
    source = script.read_text(encoding="utf-8")
    backend_guidance = (REPO_ROOT / "core" / "AGENTS.md").read_text(encoding="utf-8")
    benchmark_docs = (REPO_ROOT / "docs" / "benchmarks.md").read_text(encoding="utf-8")
    research = (
        REPO_ROOT / "docs" / "research" / "2118-bug048-script-environment-drift-2026-09-25.md"
    ).read_text(encoding="utf-8")
    rebase_notes = (REPO_ROOT / "docs" / "rebase-notes.md").read_text(encoding="utf-8")
    server_py = (
        REPO_ROOT / "mcp-server" / "vmaf-mcp" / "src" / "vmaf_mcp" / "server.py"
    ).read_text(encoding="utf-8")
    backend_perf = (REPO_ROOT / "docs" / "development" / "backend-perf-baselines.md").read_text(
        encoding="utf-8"
    )

    assert "/home/kilian/dev/vmaf" not in source
    assert "vulkan" not in source.lower()
    assert "1080p_5f" not in source
    assert "CPU 14-15, CUDA 11-12, SYCL ~34" not in source
    assert "CPU emits 14–15 keys" not in backend_guidance
    assert 'bench output ("CPU 15 keys, CUDA 12 keys, SYCL 34 keys")' not in benchmark_docs
    assert "observed `~34` intermediates" not in research
    assert "per-row metrics-key counts (CPU=15, CUDA=12" not in rebase_notes
    assert "metrics` key counts (CPU 14-15" not in rebase_notes
    assert "current CPU 14-15 / CUDA 11-12 / SYCL ~34" not in rebase_notes
    assert "CPU 14-15, CUDA 11-12" not in server_py
    assert "CPU 14-15" not in backend_perf
    assert "| Vulkan |" not in backend_guidance
    assert 'SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"' in source
    assert 'REPO_ROOT="${VMAF_ROOT:-$(cd -- "${SCRIPT_DIR}/.." && pwd)}"' in source
    subprocess.run(["bash", "-n", str(script)], check=True)


def test_benchmark_harness_resolves_root_from_script_path(tmp_path: Path) -> None:
    script = REPO_ROOT / "testdata" / "bench_all.sh"
    fake_vmaf = tmp_path / "fake-vmaf"
    invocation_log = tmp_path / "invocations.txt"
    oneapi_stub = tmp_path / "setvars.sh"
    output_dir = tmp_path / "output"
    fake_vmaf.write_text(
        """#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

args = sys.argv[1:]
output = Path(args[args.index("--output") + 1])
payload = {
    "frames": [{"metrics": {"vmaf": 80.0}}],
    "pooled_metrics": {"vmaf": {"mean": 80.0}},
}
output.write_text(json.dumps(payload), encoding="utf-8")
with Path(os.environ["FAKE_BENCH_LOG"]).open("a", encoding="utf-8") as log:
    log.write(f"{Path.cwd()}\\n")
""",
        encoding="utf-8",
    )
    fake_vmaf.chmod(0o755)
    oneapi_stub.write_text(":\n", encoding="utf-8")

    env = os.environ.copy()
    env.pop("VMAF_ROOT", None)
    env.update(
        {
            "FAKE_BENCH_LOG": str(invocation_log),
            "VMAF_BENCH_OUTDIR": str(output_dir),
            "VMAF_BIN": str(fake_vmaf),
            "VMAF_ONEAPI_SETVARS": str(oneapi_stub),
        }
    )
    result = subprocess.run(
        ["bash", str(script)],
        cwd=tmp_path,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout=30,
    )

    assert result.returncode == 0, result.stderr
    invocation_roots = invocation_log.read_text(encoding="utf-8").splitlines()
    assert invocation_roots == [str(REPO_ROOT)] * EXPECTED_BENCHMARK_CALLS
    assert result.stdout.count("FALLBACK-SUSPECT") == EXPECTED_FALLBACK_WARNINGS


def test_extract_ugc_manifest(tmp_path: Path) -> None:
    mod = _load_script("extract_ugc_features")
    report = tmp_path / "ugc.manifest.json"
    args = argparse.Namespace(
        manifest=tmp_path / "content.json",
        yuv_dir=tmp_path / "yuv",
        vmaf_bin=tmp_path / "vmaf",
        model=tmp_path / "model.json",
        out_parquet=tmp_path / "ugc.parquet",
        max_height=360,
        max_frames=300,
        threads=8,
        keep_yuv=False,
        manifest_out=report,
    )

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--manifest", str(args.manifest)],
        manifest_items=2,
        pair_count=3,
        fail_count=1,
        row_count=90,
        source_count=3,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "ugc-full-feature-extraction-manifest-v1"
    assert payload["pair_count"] == 3
    assert payload["fail_count"] == 1
    assert payload["teacher_model"] == mod.DEFAULT_MODEL
    assert "vmaf" in payload["feature_columns"]
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_extract_konvid_frames_manifest(tmp_path: Path) -> None:
    mod = _load_script("extract_konvid_frames")
    report = tmp_path / "konvid_frames.manifest.json"
    args = argparse.Namespace(root=tmp_path / "konvid", target_hw=224, manifest_out=report)

    mod._write_manifest(
        path=report,
        args=args,
        raw_argv=["--root", str(args.root)],
        root=args.root,
        manifest_entries=10,
        processed_count=8,
        missing_count=1,
        error_count=1,
        c2_rows=8,
        c3_rows=8,
    )

    payload = json.loads(report.read_text(encoding="utf-8"))
    assert payload["schema"] == "konvid-frame-extraction-manifest-v1"
    assert payload["processed_count"] == 8
    assert payload["missing_count"] == 1
    assert payload["target_hw"] == 224
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
