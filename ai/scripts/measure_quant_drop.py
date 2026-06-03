#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Measure fp32-vs-int8 drift for a quantised tiny-AI model (T5-3b / ADR-0174).

Drives both the fp32 ONNX and the matching ``<basename>.int8.onnx``
through ONNX Runtime CPU on a deterministic synthetic input set,
collects the headline-output Pearson-linear-correlation, and asserts
the drop is below the per-model budget declared in
``model/tiny/registry.json`` (``quant_accuracy_budget_plcc``).

Used by the ``ai-quant-accuracy`` CI gate. Exits 0 on pass, 1 on
budget-violation, 2 on any other error.

Usage::

    python ai/scripts/measure_quant_drop.py model/tiny/learned_filter_v1.onnx
    python ai/scripts/measure_quant_drop.py --all   # iterate every quantised model in registry
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

REGISTRY = REPO_ROOT / "model" / "tiny" / "registry.json"
SEED = 0
N_SAMPLES = 16


def _load_registry() -> dict[str, Any]:
    return json.loads(REGISTRY.read_text())


def _onnx_paths_for(entry: dict[str, Any]) -> tuple[Path, Path]:
    onnx_rel = entry["onnx"]
    fp32 = REPO_ROOT / "model" / "tiny" / onnx_rel
    int8 = fp32.with_name(fp32.stem + ".int8.onnx")
    return fp32, int8


def _measure(fp32: Path, int8: Path) -> tuple[float, float, float]:
    import numpy as np
    import onnxruntime as ort

    s_fp32 = ort.InferenceSession(str(fp32), providers=["CPUExecutionProvider"])
    s_int8 = ort.InferenceSession(str(int8), providers=["CPUExecutionProvider"])

    inp = s_fp32.get_inputs()[0]
    out_name = s_fp32.get_outputs()[0].name
    static_shape = tuple(d if isinstance(d, int) and d > 0 else 1 for d in inp.shape)

    rng = np.random.default_rng(SEED)
    accum_fp = []
    accum_q = []
    worst_max_abs = 0.0
    for _i in range(N_SAMPLES):
        x = rng.random(static_shape, dtype=np.float32)
        y_fp = s_fp32.run([out_name], {inp.name: x})[0]
        y_q = s_int8.run([out_name], {inp.name: x})[0]
        worst_max_abs = max(worst_max_abs, float(np.abs(y_fp - y_q).max()))
        accum_fp.append(y_fp.ravel())
        accum_q.append(y_q.ravel())

    all_fp = np.concatenate(accum_fp)
    all_q = np.concatenate(accum_q)
    plcc = float(np.corrcoef(all_fp, all_q)[0, 1])
    drop = 1.0 - plcc
    return plcc, drop, worst_max_abs


def _gate_one(entry: dict[str, Any]) -> dict[str, Any]:
    if entry.get("quant_mode", "fp32") == "fp32":
        print(f"[skip] {entry['id']} — quant_mode=fp32, no quantised model to gate")
        return {
            "id": entry["id"],
            "quant_mode": entry.get("quant_mode", "fp32"),
            "status": "skipped_fp32",
            "ok": True,
        }
    fp32, int8 = _onnx_paths_for(entry)
    if not fp32.is_file() or not int8.is_file():
        print(f"[FAIL] {entry['id']} — missing fp32 ({fp32.is_file()}) or int8 ({int8.is_file()})")
        return {
            "id": entry["id"],
            "quant_mode": entry.get("quant_mode"),
            "status": "missing_model",
            "ok": False,
            "fp32_exists": fp32.is_file(),
            "int8_exists": int8.is_file(),
            "fp32_path": str(fp32),
            "int8_path": str(int8),
        }
    budget = float(entry.get("quant_accuracy_budget_plcc", 0.01))
    plcc, drop, worst = _measure(fp32, int8)
    ok = drop <= budget
    status = "PASS" if ok else "FAIL"
    print(
        f"[{status}] {entry['id']:<24} mode={entry['quant_mode']:<7} "
        f"PLCC={plcc:.6f}  drop={drop:.6f}  budget={budget:.4f}  worst_abs={worst:.4f}"
    )
    return {
        "id": entry["id"],
        "quant_mode": entry["quant_mode"],
        "status": status.lower(),
        "ok": ok,
        "plcc": plcc,
        "drop": drop,
        "budget": budget,
        "worst_abs": worst,
        "fp32_path": str(fp32),
        "int8_path": str(int8),
    }


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("onnx", nargs="?", type=Path, help="Path to fp32 ONNX (default: --all)")
    parser.add_argument(
        "--all", action="store_true", help="Iterate every quantised model in the registry"
    )
    parser.add_argument(
        "--out-json",
        type=Path,
        default=None,
        help="Optional JSON gate report with ADR-0661 run provenance.",
    )
    args = parser.parse_args(raw_argv)

    try:
        reg = _load_registry()
    except Exception as exc:
        print(f"failed to load registry: {exc}", file=sys.stderr)
        return 2

    if args.all or args.onnx is None:
        results = [_gate_one(m) for m in reg["models"]]
        ok = all(bool(result["ok"]) for result in results)
        if args.out_json is not None:
            write_manifest_json(
                args.out_json,
                {
                    "gate_pass": ok,
                    "models": results,
                    "run_provenance": build_run_provenance(
                        entrypoint=SCRIPT_PATH,
                        repo_root=REPO_ROOT,
                        argv=raw_argv,
                        args=args,
                        inputs={"registry": REGISTRY},
                        outputs={"report": args.out_json},
                    ),
                },
            )
        return 0 if ok else 1

    try:
        target = str(args.onnx.resolve().relative_to(REPO_ROOT / "model" / "tiny"))
    except ValueError:
        print(f"input must live under {REPO_ROOT / 'model' / 'tiny'}: {args.onnx}", file=sys.stderr)
        return 2
    for m in reg["models"]:
        if m["onnx"] == target:
            result = _gate_one(m)
            if args.out_json is not None:
                write_manifest_json(
                    args.out_json,
                    {
                        "gate_pass": bool(result["ok"]),
                        "models": [result],
                        "run_provenance": build_run_provenance(
                            entrypoint=SCRIPT_PATH,
                            repo_root=REPO_ROOT,
                            argv=raw_argv,
                            args=args,
                            inputs={"registry": REGISTRY, "model": args.onnx.resolve()},
                            outputs={"report": args.out_json},
                        ),
                    },
                )
            return 0 if result["ok"] else 1
    print(f"no registry entry for {target}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
