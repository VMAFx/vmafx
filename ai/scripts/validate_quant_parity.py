#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Deterministic clip-level fp32-vs-int8 quantization parity gate (ADR-0207 / Research-2029 §6).

Evaluates fp32 and int8 ONNX models on real extracted clip feature data
(defaulting to ``testdata/scores_cpu_576.json``) and verifies that drift
remains within Research-2029 §6 acceptance thresholds:
- Mean absolute delta <= 0.10 VMAF points
- Maximum single-frame absolute delta <= 0.50 VMAF points
- Pearson linear correlation (PLCC) >= 0.990

Exit codes:
  0: all evaluated models pass all thresholds
  1: one or more models breach a parity threshold
  2: usage or runtime/file error
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True, include_ai_src=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root


def _collect_cli_argv(argv: list[str] | None = None) -> list[str]:
    source = sys.argv[1:] if argv is None else argv
    return [str(item) for item in source]


def _make_argument_parser(
    *,
    description: str | None = None,
    prog: str | None = None,
) -> argparse.ArgumentParser:
    return argparse.ArgumentParser(
        prog=prog,
        description=description,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )


def _build_run_provenance(
    *,
    entrypoint: Path,
    repo_root: Path,
    argv: list[str],
    args: argparse.Namespace,
) -> dict[str, Any]:
    try:
        rel_entry = str(entrypoint.resolve().relative_to(repo_root.resolve()))
    except ValueError:
        rel_entry = str(entrypoint)
    raw_args = dict(vars(args))
    norm_args = {k: str(v) if isinstance(v, Path) else v for k, v in sorted(raw_args.items())}
    return {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {
            "path": rel_entry,
            "exists": entrypoint.is_file(),
            "kind": "file",
        },
        "argv": [str(x) for x in argv],
        "args": norm_args,
    }


def _write_manifest_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(f"{path.suffix}.tmp.{os.getpid()}")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    tmp.replace(path)


REGISTRY_PATH = REPO_ROOT / "model" / "tiny" / "registry.json"
DEFAULT_FEATURES_PATH = REPO_ROOT / "testdata" / "scores_cpu_576.json"

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)

DEFAULT_MAX_MEAN_DELTA = 0.10
DEFAULT_MAX_SINGLE_DELTA = 0.50
DEFAULT_MIN_PLCC = 0.990


@dataclass(frozen=True)
class ParityThresholds:
    max_mean_delta: float = DEFAULT_MAX_MEAN_DELTA
    max_single_delta: float = DEFAULT_MAX_SINGLE_DELTA
    min_plcc: float = DEFAULT_MIN_PLCC


@dataclass
class ModelParityResult:
    model_id: str
    fp32_path: str
    int8_path: str
    n_frames: int
    mean_abs_delta: float
    max_abs_delta: float
    plcc: float
    pass_mean: bool
    pass_max: bool
    pass_plcc: bool
    ok: bool
    fp32_sample: list[float]
    int8_sample: list[float]
    gt_plcc_fp32: float | None = None
    gt_plcc_int8: float | None = None


def _load_registry() -> dict[str, Any]:
    data: dict[str, Any] = json.loads(REGISTRY_PATH.read_text())
    return data


def _extract_canonical_row(metrics: dict[str, Any]) -> list[float]:
    row: list[float] = []
    for k in CANONICAL_6:
        val = metrics.get(k)
        if val is None:
            val = metrics.get(f"integer_{k}")
        if val is None:
            raise KeyError(
                f"Missing canonical feature '{k}' (or 'integer_{k}') in metrics: {list(metrics.keys())}"
            )
        row.append(float(val))
    return row


def _load_json_features(path: Path) -> tuple[np.ndarray, np.ndarray | None]:
    data = json.loads(path.read_text())
    frames = data.get("frames", data) if isinstance(data, dict) else data
    if not isinstance(frames, list):
        raise ValueError(
            f"Unrecognized JSON structure in {path}: expected 'frames' list or list of records"
        )
    rows: list[list[float]] = []
    gt_vals: list[float] = []
    has_gt = True
    for item in frames:
        metrics = item.get("metrics", item) if isinstance(item, dict) else {}
        rows.append(_extract_canonical_row(metrics))
        gt = metrics.get("vmaf", metrics.get("mos"))
        if gt is not None:
            gt_vals.append(float(gt))
        else:
            has_gt = False
    x = np.asarray(rows, dtype=np.float32)
    gt_arr = np.asarray(gt_vals, dtype=np.float32) if has_gt and len(gt_vals) == len(rows) else None
    return x, gt_arr


def _load_features(path: Path) -> tuple[np.ndarray, np.ndarray | None]:
    suffix = path.suffix.lower()
    if suffix == ".json":
        return _load_json_features(path)
    if suffix == ".parquet":
        import pandas as pd  # type: ignore[import-untyped]

        df = pd.read_parquet(path)
        cols = [c if c in df.columns else f"integer_{c}" for c in CANONICAL_6]
        x = df[cols].values.astype(np.float32)
        gt_col = next((c for c in ("vmaf", "mos", "dmos") if c in df.columns), None)
        gt_arr = df[gt_col].values.astype(np.float32) if gt_col else None
        return x, gt_arr
    if suffix in (".npz", ".npy"):
        loaded = np.load(path)
        arr = (
            loaded["features"]
            if isinstance(loaded, np.lib.npyio.NpzFile) and "features" in loaded
            else loaded
        )
        return arr.astype(np.float32), None
    raise ValueError(
        f"Unsupported feature file format '{suffix}': expected .json, .parquet, or .npz"
    )


def _is_tabular_entry(entry: dict[str, Any], tiny_dir: Path) -> bool:
    if entry.get("quant_mode", "fp32") == "fp32":
        return False
    if entry.get("kind") != "fr":
        return False
    sidecar = tiny_dir / Path(entry["onnx"]).with_suffix(".json")
    if sidecar.is_file():
        try:
            sidecar_data = json.loads(sidecar.read_text())
            shape = sidecar_data.get("input_shape", [])
            if len(shape) == 2 and shape[-1] == 6:
                return True
        except Exception:
            pass
    onnx_file = tiny_dir / entry["onnx"]
    if onnx_file.is_file():
        try:
            import onnxruntime as ort

            sess = ort.InferenceSession(str(onnx_file), providers=["CPUExecutionProvider"])
            inp = sess.get_inputs()[0]
            if len(inp.shape) == 2 and inp.shape[-1] in (6, "6"):
                return True
        except Exception:
            pass
    return False


def _resolve_model_targets(
    args: argparse.Namespace,
) -> list[tuple[str, Path, Path]]:
    if args.fp32 is not None and args.int8 is not None:
        fp32 = args.fp32.resolve()
        int8 = args.int8.resolve()
        model_id = args.model_id or (
            fp32.stem[: -len(".onnx")] if fp32.name.endswith(".onnx") else fp32.stem
        )
        return [(model_id, fp32, int8)]

    reg = _load_registry()
    tiny_dir = REPO_ROOT / "model" / "tiny"
    target_id = args.model or args.onnx
    if target_id is not None:
        target_str = str(target_id)
        for m in reg.get("models", []):
            if m["id"] == target_str or m["onnx"] == target_str:
                fp32 = tiny_dir / m["onnx"]
                int8 = fp32.with_name(fp32.stem + ".int8.onnx")
                return [(m["id"], fp32, int8)]
        path_candidate = Path(target_id).resolve()
        if path_candidate.is_file():
            fp32 = path_candidate
            int8 = fp32.with_name(fp32.stem + ".int8.onnx")
            return [(fp32.stem, fp32, int8)]
        raise ValueError(f"Model '{target_id}' not found in registry or on disk")

    targets: list[tuple[str, Path, Path]] = []
    for m in reg.get("models", []):
        if _is_tabular_entry(m, tiny_dir):
            fp32 = tiny_dir / m["onnx"]
            int8 = fp32.with_name(fp32.stem + ".int8.onnx")
            targets.append((m["id"], fp32, int8))
        elif m.get("quant_mode", "fp32") != "fp32":
            print(
                f"[skip] {m['id']} — kind='{m.get('kind')}' with non-tabular I/O; clip parity gate targets FR models"
            )
    return targets


def _ort_predict(onnx_path: Path, x: np.ndarray) -> np.ndarray:
    import onnxruntime as ort

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    inp_name = sess.get_inputs()[0].name
    out_name = sess.get_outputs()[0].name
    return sess.run([out_name], {inp_name: x})[0].ravel().astype(np.float64)


def _evaluate_model(
    fp32_path: Path,
    int8_path: Path,
    features: np.ndarray,
    model_id: str,
    thresholds: ParityThresholds,
    gt: np.ndarray | None,
) -> ModelParityResult:
    if not fp32_path.is_file():
        raise FileNotFoundError(f"Missing fp32 model: {fp32_path}")
    if not int8_path.is_file():
        raise FileNotFoundError(f"Missing int8 model: {int8_path}")

    y_fp = _ort_predict(fp32_path, features)
    y_int = _ort_predict(int8_path, features)

    abs_diff = np.abs(y_fp - y_int)
    mean_abs = float(np.mean(abs_diff))
    max_abs = float(np.max(abs_diff))
    plcc = float(np.corrcoef(y_fp, y_int)[0, 1])

    pass_mean = mean_abs <= thresholds.max_mean_delta
    pass_max = max_abs <= thresholds.max_single_delta
    pass_plcc = plcc >= thresholds.min_plcc
    ok = pass_mean and pass_max and pass_plcc

    gt_fp = float(np.corrcoef(y_fp, gt)[0, 1]) if gt is not None and len(gt) == len(y_fp) else None
    gt_int = (
        float(np.corrcoef(y_int, gt)[0, 1]) if gt is not None and len(gt) == len(y_int) else None
    )

    return ModelParityResult(
        model_id=model_id,
        fp32_path=str(fp32_path),
        int8_path=str(int8_path),
        n_frames=len(y_fp),
        mean_abs_delta=mean_abs,
        max_abs_delta=max_abs,
        plcc=plcc,
        pass_mean=pass_mean,
        pass_max=pass_max,
        pass_plcc=pass_plcc,
        ok=ok,
        fp32_sample=[round(float(v), 4) for v in y_fp[:5]],
        int8_sample=[round(float(v), 4) for v in y_int[:5]],
        gt_plcc_fp32=gt_fp,
        gt_plcc_int8=gt_int,
    )


def _format_console_report(
    results: list[ModelParityResult],
    thresholds: ParityThresholds,
    n_frames: int,
    features_path: Path,
) -> str:
    lines = [
        "=" * 80,
        "Quantization Parity Gate (Research-2029 §6)",
        f"Features:   {features_path} ({n_frames} frames)",
        (
            f"Thresholds: Mean |Δ| <= {thresholds.max_mean_delta:.4f}, "
            f"Max |Δ| <= {thresholds.max_single_delta:.4f}, "
            f"PLCC >= {thresholds.min_plcc:.4f}"
        ),
        "=" * 80,
    ]
    for res in results:
        status_mean = "PASS" if res.pass_mean else f"FAIL (> {thresholds.max_mean_delta:.4f})"
        status_max = "PASS" if res.pass_max else f"FAIL (> {thresholds.max_single_delta:.4f})"
        status_plcc = "PASS" if res.pass_plcc else f"FAIL (< {thresholds.min_plcc:.4f})"
        overall = "PASS" if res.ok else "FAIL"
        lines.append(f"Model: {res.model_id}")
        lines.append(f"  Mean |Δ|:  {res.mean_abs_delta:.6f}  [{status_mean}]")
        lines.append(f"  Max |Δ|:   {res.max_abs_delta:.6f}  [{status_max}]")
        lines.append(f"  PLCC:      {res.plcc:.6f}  [{status_plcc}]")
        if res.gt_plcc_fp32 is not None and res.gt_plcc_int8 is not None:
            lines.append(f"  GT PLCC:   fp32={res.gt_plcc_fp32:.6f}  int8={res.gt_plcc_int8:.6f}")
        lines.append(f"  Status:    {overall}")
        lines.append("-" * 80)

    n_pass = sum(1 for r in results if r.ok)
    overall_ok = n_pass == len(results)
    lines.append(
        f"Overall Parity Gate: {'PASS' if overall_ok else 'FAIL'} ({n_pass}/{len(results)} passed)"
    )
    if not overall_ok:
        lines.append(
            "NOTE: Shipped models are dynamic PTQ. Retraining under QAT (Epic #1246) is required"
        )
        lines.append(
            "      to satisfy Research-2029 §6 static/QAT parity thresholds on real feature clips."
        )
    lines.append("=" * 80)
    return "\n".join(lines)


def _write_output_json(
    out_path: Path,
    results: list[ModelParityResult],
    thresholds: ParityThresholds,
    features_path: Path,
    n_frames: int,
    raw_argv: list[str],
    args: argparse.Namespace,
) -> None:
    manifest_data = {
        "gate_pass": all(r.ok for r in results),
        "thresholds": asdict(thresholds),
        "features_path": str(features_path.resolve()),
        "n_frames": n_frames,
        "models": [asdict(r) for r in results],
        "run_provenance": _build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
        ),
    }
    _write_manifest_json(out_path, manifest_data)


def build_parser() -> argparse.ArgumentParser:
    parser = _make_argument_parser(
        description=__doc__,
        prog="validate_quant_parity.py",
    )
    parser.add_argument("onnx", nargs="?", type=str, help="Optional model ID or path to fp32 ONNX")
    parser.add_argument("--model", type=str, default=None, help="Target model ID or path")
    parser.add_argument("--all", action="store_true", help="Gate all quantised tabular FR models")
    parser.add_argument(
        "--fp32", type=Path, default=None, help="Direct fp32 ONNX override (requires --int8)"
    )
    parser.add_argument(
        "--int8", type=Path, default=None, help="Direct int8 ONNX override (requires --fp32)"
    )
    parser.add_argument(
        "--id", dest="model_id", default=None, help="Label for --fp32/--int8 override"
    )
    parser.add_argument(
        "--features",
        type=Path,
        default=DEFAULT_FEATURES_PATH,
        help="Feature file (.json, .parquet, .npz) for evaluation",
    )
    parser.add_argument(
        "--max-mean-delta",
        type=float,
        default=DEFAULT_MAX_MEAN_DELTA,
        help=f"Mean absolute delta threshold in VMAF points (default {DEFAULT_MAX_MEAN_DELTA})",
    )
    parser.add_argument(
        "--max-single-delta",
        type=float,
        default=DEFAULT_MAX_SINGLE_DELTA,
        help=f"Maximum single-frame absolute delta in VMAF points (default {DEFAULT_MAX_SINGLE_DELTA})",
    )
    parser.add_argument(
        "--min-plcc",
        type=float,
        default=DEFAULT_MIN_PLCC,
        help=f"Minimum Pearson linear correlation coefficient (default {DEFAULT_MIN_PLCC})",
    )
    parser.add_argument(
        "--out-json", type=Path, default=None, help="Optional output JSON report path"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    raw_argv = _collect_cli_argv(argv)
    parser = build_parser()
    args = parser.parse_args(raw_argv)

    if (args.fp32 is None) != (args.int8 is None):
        print("Error: --fp32 and --int8 must be specified together", file=sys.stderr)
        return 2

    thresholds = ParityThresholds(
        max_mean_delta=args.max_mean_delta,
        max_single_delta=args.max_single_delta,
        min_plcc=args.min_plcc,
    )

    try:
        targets = _resolve_model_targets(args)
    except Exception as exc:
        print(f"Error resolving model targets: {exc}", file=sys.stderr)
        return 2

    if not targets:
        print("Error: No qualifying models found to validate", file=sys.stderr)
        return 2

    try:
        features_path = args.features.resolve()
        features, gt = _load_features(features_path)
    except Exception as exc:
        print(f"Error loading features from {args.features}: {exc}", file=sys.stderr)
        return 2

    results: list[ModelParityResult] = []
    for model_id, fp32, int8 in targets:
        try:
            res = _evaluate_model(fp32, int8, features, model_id, thresholds, gt)
            results.append(res)
        except Exception as exc:
            print(f"Error evaluating model {model_id}: {exc}", file=sys.stderr)
            return 2

    report_text = _format_console_report(results, thresholds, len(features), features_path)
    print(report_text)

    if args.out_json is not None:
        _write_output_json(
            args.out_json, results, thresholds, features_path, len(features), raw_argv, args
        )

    all_passed = all(r.ok for r in results)
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
