#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
"""Held-out test-partition validator for the CHUG HDR MOS head.

Loads a CHUG MOS head ONNX, filters CHUG feature JSONL shards to
``split == "test"`` rows only (the 552-row held-out partition that was
never used during training or validation), runs ONNX inference, and
reports PLCC / SROCC / RMSE vs the ``mos`` column.

Production gate (exit 0 on pass, exit 2 on fail):

* PLCC  >= 0.85
* SROCC >= 0.82
* RMSE  <= 0.45 MOS units

Emits a JSON report, a Markdown report, and a run-manifest sidecar.

Reproducer::

    python ai/scripts/validate_chug_hdr_mos_head.py \\
        --onnx .workingdir2/training/models/chug_hdr_mos_head_v1_wide_seed20260521.onnx \\
        --out-json  .workingdir2/training/validation/chug_held_out_test_20260527.json \\
        --out-md    .workingdir2/training/validation/chug_held_out_test_20260527.md

Exit codes:

* 0 — held-out gate PASSED
* 1 — input / inference error (not a gate verdict)
* 2 — held-out gate FAILED
"""

from __future__ import annotations

import math
import os
import sys
from pathlib import Path
from typing import Any

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_ai_scripts=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from train_konvid_mos_head import (  # noqa: E402
    CHUG_HDR_FEATURE_COLUMNS,
    FEATURE_SCHEMA_CHUG_HDR_WIDE_V1,
    N_ENCODERS,
    _load_jsonl,
    _normalise_split,
    _row_to_features,
)

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import write_run_manifest  # noqa: E402

# ---------------------------------------------------------------------------
# Gate thresholds — mirrors ADR-0325 production-flip gate; never lowered.
# ---------------------------------------------------------------------------

GATE_PLCC_MIN: float = 0.85
GATE_SROCC_MIN: float = 0.82
GATE_RMSE_MAX: float = 0.45

DEFAULT_ONNX = Path(
    os.environ.get(
        "VMAF_CHUG_HDR_ONNX",
        str(
            REPO_ROOT
            / ".workingdir2"
            / "training"
            / "models"
            / "chug_hdr_mos_head_v1_wide_seed20260521.onnx"
        ),
    )
)
DEFAULT_SHARD_DIR = REPO_ROOT / ".corpus" / "chug" / "training" / "fr_canonical_shards" / "output"
DEFAULT_OUT_DIR = REPO_ROOT / ".workingdir2" / "training" / "validation"

MANIFEST_SCHEMA = "chug-hdr-held-out-test-validator-v1"


# ---------------------------------------------------------------------------
# Corpus loading — filter to test split.
# ---------------------------------------------------------------------------


def _discover_shards(shard_dir: Path) -> list[Path]:
    """Return deterministic list of feature JSONL shards."""
    return sorted(shard_dir.glob("shard_*.features.jsonl"))


def _load_test_rows(
    shard_paths: list[Path],
) -> tuple[list[Any], list[float]]:
    """Load all ``split == "test"`` rows; return (feature_vectors, mos_labels).

    Rows that do not carry a valid MOS value or belong to a non-test split
    are silently dropped (consistent with the training-side filter in
    ``_row_to_features``).
    """
    feature_list: list[Any] = []
    mos_list: list[float] = []

    for shard_path in shard_paths:
        for raw_row in _load_jsonl(shard_path):
            if _normalise_split(raw_row.get("split")) != "test":
                continue
            result = _row_to_features(
                raw_row,
                feature_columns=CHUG_HDR_FEATURE_COLUMNS,
            )
            if result is None:
                continue
            feats, mos_f = result
            feature_list.append(feats)
            mos_list.append(mos_f)

    return feature_list, mos_list


# ---------------------------------------------------------------------------
# Metrics.
# ---------------------------------------------------------------------------


def _plcc(a: "Any", b: "Any") -> float:
    import numpy as np

    a_arr = np.asarray(a, dtype=np.float64)
    b_arr = np.asarray(b, dtype=np.float64)
    if len(a_arr) < 2 or a_arr.std() == 0.0 or b_arr.std() == 0.0:
        return float("nan")
    return float(np.corrcoef(a_arr, b_arr)[0, 1])


def _srocc(a: "Any", b: "Any") -> float:
    import numpy as np

    a_arr = np.asarray(a, dtype=np.float64)
    b_arr = np.asarray(b, dtype=np.float64)
    if len(a_arr) < 2:
        return float("nan")
    ra = np.argsort(np.argsort(a_arr))
    rb = np.argsort(np.argsort(b_arr))
    return _plcc(ra, rb)


def _rmse(a: "Any", b: "Any") -> float:
    import numpy as np

    a_arr = np.asarray(a, dtype=np.float64)
    b_arr = np.asarray(b, dtype=np.float64)
    if len(a_arr) == 0:
        return float("nan")
    return float(np.sqrt(np.mean((a_arr - b_arr) ** 2)))


# ---------------------------------------------------------------------------
# ONNX inference.
# ---------------------------------------------------------------------------


def _onnx_inference(
    onnx_path: Path,
    features: "Any",
    encoder: "Any",
) -> "Any":
    """Run ONNX inference; returns predicted MOS as a 1-D float32 array."""
    import numpy as np

    try:
        import onnxruntime as ort
    except ImportError as exc:
        raise RuntimeError("onnxruntime is required: pip install onnxruntime") from exc

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    feed = {
        "features": np.asarray(features, dtype=np.float32),
        "encoder_onehot": np.asarray(encoder, dtype=np.float32),
    }
    out = sess.run(None, feed)[0]
    return np.asarray(out, dtype=np.float32).reshape(-1)


# ---------------------------------------------------------------------------
# Report writers.
# ---------------------------------------------------------------------------


def _build_gate_verdict(plcc: float, srocc: float, rmse: float) -> dict[str, Any]:
    passed = (
        (not math.isnan(plcc))
        and plcc >= GATE_PLCC_MIN
        and (not math.isnan(srocc))
        and srocc >= GATE_SROCC_MIN
        and (not math.isnan(rmse))
        and rmse <= GATE_RMSE_MAX
    )
    return {
        "passed": bool(passed),
        "plcc": plcc,
        "srocc": srocc,
        "rmse": rmse,
        "thresholds": {
            "plcc_min": GATE_PLCC_MIN,
            "srocc_min": GATE_SROCC_MIN,
            "rmse_max": GATE_RMSE_MAX,
        },
    }


def _write_json_report(
    path: Path,
    *,
    args: Any,
    raw_argv: list[str],
    n_rows: int,
    plcc: float,
    srocc: float,
    rmse: float,
    gate: dict[str, Any],
    sample_pred: list[float],
    sample_mos: list[float],
    shard_paths: list[Path],
) -> None:
    sections: dict[str, Any] = {
        "corpus": "chug-hdr",
        "split": "test",
        "n_test_rows": n_rows,
        "feature_schema": FEATURE_SCHEMA_CHUG_HDR_WIDE_V1,
        "metrics": {
            "plcc": plcc,
            "srocc": srocc,
            "rmse": rmse,
        },
        "gate": gate,
        "sample_predictions": sample_pred,
        "sample_mos": sample_mos,
    }
    write_run_manifest(
        path,
        schema=MANIFEST_SCHEMA,
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={
            "onnx": args.onnx,
            "shards": shard_paths,
        },
        outputs={
            "json": args.out_json,
            "md": args.out_md,
        },
        sections=sections,
    )


def _write_md_report(
    path: Path,
    *,
    n_rows: int,
    plcc: float,
    srocc: float,
    rmse: float,
    gate: dict[str, Any],
    onnx_path: Path,
) -> None:
    status_icon = "PASS" if gate["passed"] else "FAIL"
    lines = [
        "# CHUG HDR MOS Head — Held-Out Test Validation",
        "",
        f"**Model**: `{onnx_path.name}`",
        f"**Split**: test ({n_rows} rows)",
        f"**Feature schema**: `{FEATURE_SCHEMA_CHUG_HDR_WIDE_V1}` (34 columns)",
        "",
        "## Held-out gate result",
        "",
        f"**{status_icon}**",
        "",
        "| Metric | Value | Threshold | Result |",
        "|--------|-------|-----------|--------|",
        f"| PLCC   | {plcc:.4f} | ≥ {GATE_PLCC_MIN:.2f} | {'OK' if plcc >= GATE_PLCC_MIN else 'FAIL'} |",
        f"| SROCC  | {srocc:.4f} | ≥ {GATE_SROCC_MIN:.2f} | {'OK' if srocc >= GATE_SROCC_MIN else 'FAIL'} |",
        f"| RMSE   | {rmse:.4f} | ≤ {GATE_RMSE_MAX:.2f} | {'OK' if rmse <= GATE_RMSE_MAX else 'FAIL'} |",
        "",
        "## Notes",
        "",
        (
            "The `test` partition (552 rows) was held out during all training and"
            " validation passes. These scores are an unbiased estimate of the"
            " model's generalisation performance on unseen CHUG HDR content."
        ),
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# Entry point.
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    import numpy as np

    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="validate_chug_hdr_mos_head.py",
        description=__doc__,
    )
    ap.add_argument(
        "--onnx",
        type=Path,
        default=DEFAULT_ONNX,
        help="Path to the CHUG MOS head ONNX file.",
    )
    ap.add_argument(
        "--feature-jsonl",
        type=Path,
        action="append",
        default=[],
        help=(
            "CHUG feature JSONL shard; may be repeated. "
            "Defaults to shard_*.features.jsonl under --shard-dir."
        ),
    )
    ap.add_argument(
        "--shard-dir",
        type=Path,
        default=DEFAULT_SHARD_DIR,
        help="Directory searched for shard_*.features.jsonl when --feature-jsonl is absent.",
    )
    ap.add_argument(
        "--out-json",
        type=Path,
        default=DEFAULT_OUT_DIR / "chug_held_out_test_20260527.json",
        help="Path for the JSON validation report + run-manifest sidecar.",
    )
    ap.add_argument(
        "--out-md",
        type=Path,
        default=DEFAULT_OUT_DIR / "chug_held_out_test_20260527.md",
        help="Path for the Markdown validation report.",
    )
    ap.add_argument(
        "--gate-plcc",
        type=float,
        default=GATE_PLCC_MIN,
        help="Minimum PLCC for gate pass (default matches ADR-0325).",
    )
    ap.add_argument(
        "--gate-srocc",
        type=float,
        default=GATE_SROCC_MIN,
        help="Minimum SROCC for gate pass.",
    )
    ap.add_argument(
        "--gate-rmse",
        type=float,
        default=GATE_RMSE_MAX,
        help="Maximum RMSE for gate pass.",
    )
    args = ap.parse_args(raw_argv)

    # Resolve effective thresholds from CLI (may be overridden in tests).
    gate_plcc = args.gate_plcc
    gate_srocc = args.gate_srocc
    gate_rmse = args.gate_rmse

    # Resolve ONNX.
    if not args.onnx.is_file():
        print(
            f"[validate-chug-mos] error: ONNX not found at {args.onnx}",
            file=sys.stderr,
        )
        return 1

    # Resolve shard paths.
    shard_paths: list[Path] = list(args.feature_jsonl)
    if not shard_paths:
        shard_paths = _discover_shards(args.shard_dir)
    if not shard_paths:
        print(
            f"[validate-chug-mos] error: no feature JSONL shards found under {args.shard_dir}",
            file=sys.stderr,
        )
        return 1

    print(f"[validate-chug-mos] loading test split from {len(shard_paths)} shard(s)…")
    feature_list, mos_list = _load_test_rows(shard_paths)

    if not feature_list:
        print(
            "[validate-chug-mos] error: no test-split rows with valid MOS found",
            file=sys.stderr,
        )
        return 1

    n_rows = len(feature_list)
    print(f"[validate-chug-mos] test rows loaded: {n_rows}")

    features = np.stack(feature_list).astype(np.float32)
    encoder = np.ones((n_rows, N_ENCODERS), dtype=np.float32)
    mos_arr = np.asarray(mos_list, dtype=np.float32)

    print(f"[validate-chug-mos] running ONNX inference on {args.onnx.name}…")
    try:
        pred = _onnx_inference(args.onnx, features, encoder)
    except Exception as exc:
        print(f"[validate-chug-mos] error: ONNX inference failed: {exc}", file=sys.stderr)
        return 1

    plcc = _plcc(pred, mos_arr)
    srocc = _srocc(pred, mos_arr)
    rmse = _rmse(pred, mos_arr)

    print(
        f"[validate-chug-mos] "
        f"PLCC={plcc:.4f}  SROCC={srocc:.4f}  RMSE={rmse:.4f}"
        f"  (n={n_rows})"
    )

    # Use CLI-provided gate thresholds (supports test overrides).
    gate: dict[str, Any] = {
        "passed": bool(
            (not math.isnan(plcc))
            and plcc >= gate_plcc
            and (not math.isnan(srocc))
            and srocc >= gate_srocc
            and (not math.isnan(rmse))
            and rmse <= gate_rmse
        ),
        "plcc": plcc,
        "srocc": srocc,
        "rmse": rmse,
        "thresholds": {
            "plcc_min": gate_plcc,
            "srocc_min": gate_srocc,
            "rmse_max": gate_rmse,
        },
    }

    sample_pred = [float(v) for v in pred[:5].tolist()]
    sample_mos = [float(v) for v in mos_arr[:5].tolist()]

    # Write reports.
    if args.out_json is not None:
        _write_json_report(
            args.out_json,
            args=args,
            raw_argv=raw_argv,
            n_rows=n_rows,
            plcc=plcc,
            srocc=srocc,
            rmse=rmse,
            gate=gate,
            sample_pred=sample_pred,
            sample_mos=sample_mos,
            shard_paths=shard_paths,
        )
        print(f"[validate-chug-mos] wrote JSON report: {args.out_json}")
    if args.out_md is not None:
        _write_md_report(
            args.out_md,
            n_rows=n_rows,
            plcc=plcc,
            srocc=srocc,
            rmse=rmse,
            gate=gate,
            onnx_path=args.onnx,
        )
        print(f"[validate-chug-mos] wrote Markdown report: {args.out_md}")

    verdict = "PASS" if gate["passed"] else "FAIL"
    print(
        f"[validate-chug-mos] held-out gate: {verdict} — "
        f"PLCC={plcc:.4f} SROCC={srocc:.4f} RMSE={rmse:.4f}"
    )

    return 0 if gate["passed"] else 2


__all__ = [
    "GATE_PLCC_MIN",
    "GATE_RMSE_MAX",
    "GATE_SROCC_MIN",
    "MANIFEST_SCHEMA",
    "_build_gate_verdict",
    "_discover_shards",
    "_load_test_rows",
    "_onnx_inference",
    "_plcc",
    "_rmse",
    "_srocc",
    "main",
]


if __name__ == "__main__":
    sys.exit(main())
