#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
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
from typing import Any

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import write_run_manifest  # noqa: E402


def _parse_args(raw_argv: list[str]) -> argparse.Namespace:
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
    return parser.parse_args(raw_argv)


def _resolve_paths(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    source = args.onnx.resolve()
    if not source.is_file():
        sys.exit(f"input not found: {source}")
    calibration = args.calibration.resolve()
    if not calibration.is_file():
        sys.exit(f"calibration set not found: {calibration}")
    destination = args.output or source.with_name(source.stem + ".int8.onnx")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not os.access(destination.parent, os.W_OK):
        sys.exit(
            f"error: destination directory is not writable: {destination.parent}\n"
            f"hint: pass --output /path/to/writable/dir/{destination.name}"
        )
    return source, calibration, destination


def _write_quantization_report(
    args: argparse.Namespace,
    raw_argv: list[str],
    source: Path,
    calibration: Path,
    destination: Path,
    arrays: Any,
    input_bytes: int,
    output_bytes: int,
) -> None:
    if args.report_out is None:
        return
    first_input = next(iter(arrays.keys()))
    write_run_manifest(
        args.report_out,
        schema="ptq-static-report-v1",
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"model": source, "calibration": calibration},
        outputs={"model": destination, "report": args.report_out},
        sections={
            "mode": "static",
            "input_bytes": input_bytes,
            "output_bytes": output_bytes,
            "size_ratio": output_bytes / input_bytes,
            "per_channel": bool(args.per_channel),
            "calibration_samples": int(arrays[first_input].shape[0]),
            "calibration_inputs": sorted(arrays.keys()),
        },
    )


def _quantize(
    args: argparse.Namespace,
    raw_argv: list[str],
    source: Path,
    calibration: Path,
    destination: Path,
) -> None:

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

    print(
        f"[ptq_static] {source}  ->  {destination}  "
        f"cal={calibration}  per-channel={args.per_channel}"
    )

    # Build a CalibrationDataReader from the .npz on the fly.
    arrays = np.load(calibration, allow_pickle=False)

    class _NpzReader(CalibrationDataReader):
        def __init__(self, npz: "np.lib.npyio.NpzFile") -> None:
            self._names = list(npz.keys())
            self._n = int(npz[self._names[0]].shape[0])
            self._cursor = 0
            self._npz = npz

        def get_next(self) -> dict[str, "np.ndarray[Any, Any]"] | None:
            if self._cursor >= self._n:
                return None
            sample = {n: self._npz[n][self._cursor : self._cursor + 1] for n in self._names}
            self._cursor += 1
            return sample

    quantize_static(
        model_input=str(source),
        model_output=str(destination),
        calibration_data_reader=_NpzReader(arrays),
        # QDQ only: core/src/dnn/op_allowlist.c has no QLinear* ops; mirrors ai/src/vmaf_train/quantize.py
        quant_format=QuantFormat.QDQ,
        weight_type=QuantType.QInt8,
        activation_type=QuantType.QInt8,
        per_channel=args.per_channel,
    )
    input_bytes = source.stat().st_size
    output_bytes = destination.stat().st_size
    ratio = output_bytes / input_bytes
    print(f"[ptq_static] done — {input_bytes:,} -> {output_bytes:,} bytes ({ratio:.2f}×)")
    _write_quantization_report(
        args,
        raw_argv,
        source,
        calibration,
        destination,
        arrays,
        input_bytes,
        output_bytes,
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = _parse_args(raw_argv)
    source, calibration, destination = _resolve_paths(args)
    _quantize(args, raw_argv, source, calibration, destination)
    return 0


if __name__ == "__main__":
    sys.exit(main())
