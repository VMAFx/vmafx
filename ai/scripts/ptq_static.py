#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ONNX static post-training quantisation (ADR-0173 / T5-3).

Static PTQ runs the model on a representative calibration set to
collect activation ranges, then bakes those ranges into per-tensor
QDQ scales. Higher accuracy than dynamic PTQ; needs a calibration
set on disk.

The calibration data path is read from
``model/tiny/registry.json`` (``quant_calibration_set`` field) when
the registry contains the model id; otherwise a CLI override is
required.

Calibration format: a numpy ``.npz`` with one entry per model input
name, each containing a stack of `[N, ...]` representative inputs.
``ai/scripts/build_calibration_set.py`` (future) will produce this
from a parquet feature cache; for now operators hand-craft it.

Usage::

    python ai/scripts/ptq_static.py model/tiny/nr_metric_v1.onnx \\
        --calibration ai/calibration/nr_metric_v1.npz
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import write_run_manifest  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("onnx", type=Path, help="Path to fp32 ONNX file")
    parser.add_argument(
        "--calibration",
        type=Path,
        required=True,
        help="Calibration .npz with one stacked input array per ONNX input name",
    )
    parser.add_argument(
        "--output", type=Path, default=None, help="Output path; default appends `.int8.onnx`"
    )
    parser.add_argument("--per-channel", action="store_true")
    parser.add_argument(
        "--report-out",
        type=Path,
        default=None,
        help="Optional JSON report with calibration stats and ADR-0661 run provenance.",
    )
    args = parser.parse_args(raw_argv)

    try:
        import numpy as np
        from onnxruntime.quantization import (
            CalibrationDataReader,
            QuantFormat,
            QuantType,
            quantize_static,
        )
    except ImportError as exc:
        sys.exit(f"onnxruntime.quantization / numpy not available: {exc}")

    src = args.onnx.resolve()
    if not src.is_file():
        sys.exit(f"input not found: {src}")
    cal = args.calibration.resolve()
    if not cal.is_file():
        sys.exit(f"calibration set not found: {cal}")
    dst = args.output or src.with_name(src.stem + ".int8.onnx")
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not os.access(dst.parent, os.W_OK):
        sys.exit(
            f"error: destination directory is not writable: {dst.parent}\n"
            f"hint: pass --output /path/to/writable/dir/{dst.name}"
        )

    print(f"[ptq_static] {src}  ->  {dst}  cal={cal}  per-channel={args.per_channel}")

    # Build a CalibrationDataReader from the .npz on the fly.
    arrays = np.load(cal, allow_pickle=False)

    class _NpzReader(CalibrationDataReader):
        def __init__(self, npz: "np.lib.npyio.NpzFile") -> None:
            self._names = list(npz.keys())
            self._n = int(npz[self._names[0]].shape[0])
            self._cursor = 0
            self._npz = npz

        def get_next(self):
            if self._cursor >= self._n:
                return None
            sample = {n: self._npz[n][self._cursor : self._cursor + 1] for n in self._names}
            self._cursor += 1
            return sample

    quantize_static(
        model_input=str(src),
        model_output=str(dst),
        calibration_data_reader=_NpzReader(arrays),
        # QDQ only: core/src/dnn/op_allowlist.c has no QLinear* ops; mirrors ai/src/vmaf_train/quantize.py
        quant_format=QuantFormat.QDQ,
        weight_type=QuantType.QInt8,
        activation_type=QuantType.QInt8,
        per_channel=args.per_channel,
    )
    sz_in = src.stat().st_size
    sz_out = dst.stat().st_size
    ratio = sz_out / sz_in
    print(f"[ptq_static] done — {sz_in:,} -> {sz_out:,} bytes ({ratio:.2f}×)")
    if args.report_out is not None:
        first_input = next(iter(arrays.keys()))
        write_run_manifest(
            args.report_out,
            schema="ptq-static-report-v1",
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"model": src, "calibration": cal},
            outputs={"model": dst, "report": args.report_out},
            sections={
                "mode": "static",
                "input_bytes": sz_in,
                "output_bytes": sz_out,
                "size_ratio": ratio,
                "per_channel": bool(args.per_channel),
                "calibration_samples": int(arrays[first_input].shape[0]),
                "calibration_inputs": sorted(arrays.keys()),
            },
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
