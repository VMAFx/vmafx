#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""ONNX dynamic post-training quantisation (ADR-0173 / T5-3).

The cheapest of the three quantisation modes — quantises weights
offline; activations are quantised at runtime by ORT. Single CLI
call, no calibration data needed. Best fit for models where:

- We don't ship a calibration set,
- The deployment box differs from the training box,
- A small accuracy hit is acceptable for a 2× speedup.

Output lands at ``<input>.int8.onnx`` next to the fp32 source.
``model/tiny/registry.json`` should set ``quant_mode = "dynamic"``
on the matching entry; the libvmaf loader prefers the int8 file
when it sees that mode.

Pre-flight: ONNX models exported by ``torch.onnx.export`` (dynamo
or legacy) often duplicate every initialiser into ``graph.value_info``
with shape annotations that do not survive the dynamic-batch axis
substitution. ``onnxruntime.quantization.quantize_dynamic`` then
trips ``onnx.shape_inference`` with ``Inferred shape and existing
shape differ`` (see ADR-0174 / PR #174). We strip those duplicates
into a temporary inlined fp32 model before invoking the quantiser.

Usage::

    python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import write_run_manifest  # noqa: E402


def _save_inlined_for_quant(src: Path, dst: Path) -> None:
    """Copy ``src`` to ``dst`` with all initialisers inlined and any
    ``value_info`` entries that collide with initialiser names removed.

    This is the same workaround introduced in PR #174 (T5-3e) for
    ``vmaf_tiny_v1*.onnx`` — the ``torch.onnx`` exporter emits
    ``value_info`` records whose static-shape annotations confuse the
    quantiser's shape inference once a dynamic batch axis is involved.
    Initializers carry their own canonical shape, so dropping the
    duplicate ``value_info`` records is safe.
    """
    import onnx

    proto = onnx.load(str(src))
    init_names = {t.name for t in proto.graph.initializer}
    survivors = [vi for vi in proto.graph.value_info if vi.name not in init_names]
    if len(survivors) != len(proto.graph.value_info):
        del proto.graph.value_info[:]
        proto.graph.value_info.extend(survivors)
    onnx.save(proto, str(dst), save_as_external_data=False)


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("onnx", type=Path, help="Path to fp32 ONNX file")
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output path; default appends `.int8.onnx`",
    )
    parser.add_argument(
        "--per-channel",
        action="store_true",
        help="Per-channel weight quantisation (better accuracy, marginal "
        "size cost). Default off; flip on for sensitive models.",
    )
    parser.add_argument(
        "--report-out",
        type=Path,
        default=None,
        help="Optional JSON report with size stats and ADR-0661 run provenance.",
    )
    args = parser.parse_args(raw_argv)

    try:
        from onnxruntime.quantization import QuantType, quantize_dynamic
    except ImportError as exc:
        sys.exit(f"onnxruntime.quantization not available: {exc}")

    src = args.onnx.resolve()
    if not src.is_file():
        sys.exit(f"input not found: {src}")
    if not src.name.endswith(".onnx"):
        sys.exit(f"input must end with .onnx: {src}")
    dst = args.output or src.with_name(src.stem + ".int8.onnx")
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not os.access(dst.parent, os.W_OK):
        sys.exit(
            f"error: destination directory is not writable: {dst.parent}\n"
            f"hint: pass --output /path/to/writable/dir/{dst.name}"
        )

    print(f"[ptq_dynamic] {src}  ->  {dst}  per-channel={args.per_channel}")
    with tempfile.TemporaryDirectory(prefix="ptq_dynamic_") as tmp:
        prepped = Path(tmp) / (src.stem + ".fp32_inlined.onnx")
        _save_inlined_for_quant(src, prepped)
        quantize_dynamic(
            model_input=str(prepped),
            model_output=str(dst),
            weight_type=QuantType.QInt8,
            per_channel=args.per_channel,
        )
    sz_in = src.stat().st_size
    sz_out = dst.stat().st_size
    ratio = sz_out / sz_in
    print(f"[ptq_dynamic] done — {sz_in:,} -> {sz_out:,} bytes ({ratio:.2f}×)")
    if args.report_out is not None:
        write_run_manifest(
            args.report_out,
            schema="ptq-dynamic-report-v1",
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"model": src},
            outputs={"model": dst, "report": args.report_out},
            sections={
                "mode": "dynamic",
                "input_bytes": sz_in,
                "output_bytes": sz_out,
                "size_ratio": ratio,
                "per_channel": bool(args.per_channel),
            },
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
