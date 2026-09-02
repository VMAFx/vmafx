#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Quantization-aware training driver for tiny-AI ONNX models.

Implements the design locked in
`docs/adr/0207-tinyai-qat-design.md`. Replaces the
`NotImplementedError` scaffold landed in PR-track ADR-0173.

Pipeline
--------

1. Read a YAML config (the same shape `vmaf-train fit` consumes,
   plus an optional ``qat:`` block).
2. Build the underlying Lightning module via
   ``ai.src.vmaf_train.train.MODEL_REGISTRY``.
3. fp32 warm-start: train normally for ``--epochs-fp32`` epochs.
4. Insert FX fake-quants (``prepare_qat_fx`` with the default
   symmetric per-tensor activation + per-channel weight qconfig).
5. QAT fine-tune for ``--epochs-qat`` epochs at ``--lr-qat``
   (default fp32-lr / 10).
6. Copy QAT-conditioned weights into a fresh fp32 module, export to
   ONNX, then ORT-static-quantize using a calibration slice drawn
   from the training corpus.

Outputs
-------

* ``--output`` — the final ``<basename>.int8.onnx`` (QDQ format).
* Sibling ``<basename>.qat.fp32.onnx`` — the QAT-conditioned fp32
  bridge model. Optional artefact for diff vs the
  pre-QAT-fp32-trained baseline.

Usage
-----

::

    python ai/scripts/qat_train.py \\
        --config ai/configs/learned_filter_v1_qat.yaml \\
        --output model/tiny/learned_filter_v1.int8.onnx

The shipped config defaults to a ``--smoke`` run when invoked
without a real training corpus on disk; this is what the CI smoke
test exercises.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

# Allow this script to run both as ``python ai/scripts/qat_train.py``
# (where the ai/ package needs to be importable) and as
# ``python -m ai.scripts.qat_train``.
SCRIPT_PATH = Path(__file__).resolve()
_REPO_ROOT = SCRIPT_PATH.parents[2]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))
if str(_REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT / "ai" / "src"))

from aiutils.run_manifest import write_run_manifest  # noqa: E402


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--config",
        type=Path,
        required=True,
        help="Lightning training config YAML (e.g. ai/configs/learned_filter_v1_qat.yaml)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Output `.int8.onnx` path",
    )
    parser.add_argument(
        "--epochs-fp32",
        type=int,
        default=None,
        help="fp32 warm-start epochs (overrides config, default 20)",
    )
    parser.add_argument(
        "--epochs-qat",
        type=int,
        default=None,
        help="QAT fine-tune epochs (overrides config, default 10)",
    )
    parser.add_argument(
        "--lr-qat",
        type=float,
        default=None,
        help="QAT fine-tune learning rate (overrides config, default fp32-lr / 10)",
    )
    parser.add_argument(
        "--n-calibration",
        type=int,
        default=64,
        help="ORT static calibration sample count (default 64)",
    )
    parser.add_argument(
        "--smoke",
        action="store_true",
        help="Skip training; exercise wiring only (for CI smoke / dev round-trip)",
    )
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--report-out",
        type=Path,
        default=None,
        help="Optional JSON report with QAT outputs and ADR-0661 run provenance.",
    )
    return parser.parse_args(argv)


def _load_config(path: Path) -> dict[str, Any]:
    import yaml

    with path.open() as fh:
        return yaml.safe_load(fh) or {}


def _resolve_qat_config(cfg_doc: dict[str, Any], args: argparse.Namespace):
    """Build a QatConfig from YAML + CLI overrides."""
    from ai.train.qat import QatConfig

    qat_block = cfg_doc.get("qat", {}) or {}

    epochs_fp32 = (
        args.epochs_fp32 if args.epochs_fp32 is not None else int(qat_block.get("epochs_fp32", 20))
    )
    epochs_qat = (
        args.epochs_qat if args.epochs_qat is not None else int(qat_block.get("epochs_qat", 10))
    )
    lr_qat = args.lr_qat if args.lr_qat is not None else qat_block.get("lr_qat")
    if lr_qat is not None:
        lr_qat = float(lr_qat)

    return QatConfig(
        epochs_fp32=epochs_fp32,
        epochs_qat=epochs_qat,
        lr_qat=lr_qat,
        n_calibration=args.n_calibration,
        output_int8_onnx=args.output.resolve(),
        seed=args.seed,
        smoke=bool(args.smoke or qat_block.get("smoke", False)),
        extra={
            "lr": cfg_doc.get("model_args", {}).get("lr", 1e-4),
            "model": cfg_doc.get("model"),
            "model_args": cfg_doc.get("model_args", {}),
            "input_shape": qat_block.get("input_shape"),
            "input_name": qat_block.get("input_name", "input"),
            "output_name": qat_block.get("output_name", "output"),
        },
    )


def _build_model_factory(cfg_doc: dict[str, Any]):
    """Return a zero-arg callable that constructs the configured model.

    Importing the model registry here keeps the driver lightweight
    when ``--help`` is requested.
    """
    from ai.src.vmaf_train.train import MODEL_REGISTRY  # type: ignore[import]

    model_kind = cfg_doc.get("model")
    if model_kind not in MODEL_REGISTRY:
        raise SystemExit(
            f"unknown model kind in config: {model_kind!r}; " f"valid: {sorted(MODEL_REGISTRY)}"
        )
    model_cls = MODEL_REGISTRY[model_kind]
    model_args = cfg_doc.get("model_args", {}) or {}

    def factory():
        return model_cls(**model_args)

    return factory


def _build_example_inputs(cfg_doc: dict[str, Any], qat_cfg) -> tuple[Any, ...]:
    import torch

    qat_block = cfg_doc.get("qat", {}) or {}
    shape = qat_block.get("input_shape") or qat_cfg.extra.get("input_shape")
    if shape is None:
        # Fall back to a luma-shaped 4D tensor — matches every shipped
        # tiny-AI model today (LearnedFilter, NRMetric).
        shape = [1, 1, 32, 32]
    return (torch.zeros(tuple(int(d) for d in shape), dtype=torch.float32),)


def _build_train_loader_factory(cfg_doc: dict[str, Any], qat_cfg):
    """Best-effort training data loader for the QAT phases.

    For the smoke path this returns ``None`` — the pipeline skips
    both training phases. For real training the caller hooks up a
    parquet feature cache via the existing
    ``vmaf_train.datamodule.VmafTrainDataModule``; until that wiring
    lands per-model, the driver supports the smoke path + real
    training only when the config carries a ``cache:`` field that
    points at a non-empty parquet on disk.
    """
    if qat_cfg.smoke:
        return None

    cache_path = cfg_doc.get("cache")
    if not cache_path:
        return None
    cache = Path(cache_path)
    if not cache.is_file():
        print(
            f"[qat_train] cache {cache} missing; falling back to smoke mode",
            file=sys.stderr,
        )
        qat_cfg.smoke = True
        return None

    # Wrap the existing Lightning data module into an iterable of (x, y)
    # tensor pairs for the minimal QAT loop.
    from ai.src.vmaf_train.datamodule import VmafTrainDataModule  # type: ignore[import]

    def factory():
        dm = VmafTrainDataModule(
            cache,
            batch_size=int(cfg_doc.get("batch_size", 32)),
            val_frac=float(cfg_doc.get("val_frac", 0.1)),
            test_frac=float(cfg_doc.get("test_frac", 0.1)),
        )
        dm.setup("fit")
        return dm.train_dataloader()

    return factory


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    args = _parse_args(raw_argv)

    try:
        import torch  # noqa: F401
    except ImportError as exc:
        print(f"torch not available: {exc}", file=sys.stderr)
        return 2

    cfg_path = args.config.resolve()
    if not cfg_path.is_file():
        print(f"config not found: {cfg_path}", file=sys.stderr)
        return 2

    cfg_doc = _load_config(cfg_path)
    qat_cfg = _resolve_qat_config(cfg_doc, args)
    print(
        f"[qat_train] config={cfg_path} "
        f"epochs_fp32={qat_cfg.epochs_fp32} epochs_qat={qat_cfg.epochs_qat} "
        f"smoke={qat_cfg.smoke}"
    )

    model_factory = _build_model_factory(cfg_doc)
    example_inputs = _build_example_inputs(cfg_doc, qat_cfg)
    train_loader_factory = _build_train_loader_factory(cfg_doc, qat_cfg)

    input_name = qat_cfg.extra["input_name"]
    output_name = qat_cfg.extra["output_name"]
    dynamic_axes = {input_name: {0: "batch"}, output_name: {0: "batch"}}

    from ai.train.qat import run_qat

    result = run_qat(
        model_factory=model_factory,
        qat_cfg=qat_cfg,
        example_inputs=example_inputs,
        input_names=[input_name],
        output_names=[output_name],
        dynamic_axes=dynamic_axes,
        train_loader_factory=train_loader_factory,
    )

    print(
        f"[qat_train] done — fp32_onnx={result.fp32_onnx} "
        f"int8_onnx={result.int8_onnx} params={result.n_params}"
    )
    if args.report_out is not None:
        write_run_manifest(
            args.report_out,
            schema="qat-train-report-v1",
            entrypoint=SCRIPT_PATH,
            repo_root=_REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"config": cfg_path},
            outputs={
                "fp32_model": result.fp32_onnx,
                "int8_model": result.int8_onnx,
                "report": args.report_out,
            },
            sections={
                "mode": "qat",
                "model": cfg_doc.get("model"),
                "smoke": bool(qat_cfg.smoke),
                "epochs_fp32": int(result.epochs_fp32),
                "epochs_qat": int(result.epochs_qat),
                "n_calibration": int(qat_cfg.n_calibration),
                "n_params": int(result.n_params),
                "fp32_onnx": str(result.fp32_onnx),
                "int8_onnx": str(result.int8_onnx),
            },
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
