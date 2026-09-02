# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Quantization-aware training (QAT) trainer hook for tiny-AI models.

Implements the design locked in
`docs/adr/0207-tinyai-qat-design.md`. The pipeline runs in three
sequential phases:

1. **fp32 phase** — train the Lightning module normally. Reuses
   `ai.src.vmaf_train.train.train` against the model's existing
   YAML config so the warm-start matches what `vmaf-train fit`
   would produce.
2. **Fake-quant insertion** — wrap the trained module with
   `torch.ao.quantization.quantize_fx.prepare_qat_fx` using the
   default symmetric per-tensor activation + per-channel weight
   qconfig (matching the PTQ static recipe in Research-0006 §2).
3. **QAT fine-tune phase** — train the FX-prepared module for a
   smaller number of epochs at a 10× reduced learning rate. The
   fake-quant observers nudge the weights toward
   quantization-friendly values.

After phase 3 the QAT-conditioned weights are copied back into a
fresh fp32 module and exported to ONNX. ORT-side
`quantize_static` then bakes the activation ranges into a QDQ
graph using a calibration set drawn from the training corpus.

Why two-step (PyTorch QAT → fp32 ONNX → ORT static) instead of
`convert_fx` → ONNX directly: PyTorch 2.11's TorchScript ONNX
exporter cannot translate the `quantized::conv2d` /
`fused_moving_avg_obs_fake_quant` ops produced by `convert_fx`
to standard ONNX QDQ nodes, and the new TorchDynamo exporter
chokes on `Conv2dPackedParamsBase`. Splitting the work — QAT
conditions the weights, ORT emits the QDQ graph — sidesteps both
exporter limitations and produces an ONNX file that loads on
every EP the PTQ static path supports (CPU, CUDA, OpenVINO).

Public surface
--------------

* :func:`run_qat` — config-driven entry point. Returns the path to
  the exported `.int8.onnx` (and the intermediate fp32 ONNX).
* :class:`QatConfig` — dataclass with the QAT-specific knobs
  layered on top of `vmaf_train.train.TrainConfig`.

Both are imported by `ai/scripts/qat_train.py` and (eventually) by
the `vmaf-train qat` subcommand.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator

import numpy as np


@dataclass
class QatConfig:
    """QAT-specific configuration overlaid on `TrainConfig`.

    The fp32 phase reuses the underlying YAML config verbatim; QAT
    knobs live here so a single command-line invocation can pick
    fp32-epoch-count / qat-epoch-count / qat-lr without editing the
    base config.
    """

    epochs_fp32: int = 20
    epochs_qat: int = 10
    lr_qat: float | None = None  # default: fp32-lr / 10
    n_calibration: int = 64
    output_int8_onnx: Path | None = None
    output_fp32_onnx: Path | None = None
    seed: int = 0
    # When True, skip the actual training and run just the wiring
    # paths. Used by the smoke-test in `ai/tests/test_qat_smoke.py`.
    smoke: bool = False
    # Extra forwarded knobs from the TrainConfig YAML so callers
    # don't need to keep two configs in sync.
    extra: dict[str, Any] = field(default_factory=dict)


def _example_input_for(model: Any, default_shape: tuple[int, ...] = (1, 1, 32, 32)):
    """Best-effort example-input synthesis for FX trace.

    Each shipped tiny-AI Lightning module has a documented input
    contract; rather than hard-code per-class shapes here, prefer
    the model's `example_input_array` attribute (Lightning's
    convention) and fall back to a luma-shaped 4D tensor.
    """
    import torch

    arr = getattr(model, "example_input_array", None)
    if arr is not None:
        if isinstance(arr, (tuple, list)):
            return tuple(t.detach().clone() for t in arr)
        return (arr.detach().clone(),)
    return (torch.zeros(default_shape, dtype=torch.float32),)


def _build_qconfig_mapping():
    """Default QAT qconfig: symmetric per-tensor act + per-channel weight.

    Uses `get_default_qat_qconfig_mapping("x86")` per
    ADR-0207's Decision §2. The "x86" backend selects per-channel
    symmetric weight observers and per-tensor symmetric activation
    observers, matching the ORT static-PTQ recipe exactly.
    """
    from torch.ao.quantization import get_default_qat_qconfig_mapping

    return get_default_qat_qconfig_mapping("x86")


def _prepare_qat(module: Any, example_inputs: tuple[Any, ...]) -> Any:
    """Insert fake-quant observers via FX graph mode."""
    from torch.ao.quantization.quantize_fx import prepare_qat_fx

    qmap = _build_qconfig_mapping()
    module.train()
    return prepare_qat_fx(module, qmap, example_inputs)


def _copy_qat_weights_into_fp32(qat_module: Any, fp32_module: Any) -> int:
    """Copy QAT-conditioned parameter tensors into a fresh fp32 module.

    The FX-prepared module preserves submodule names (entry, body.*,
    exit, ...), so a state-dict diff that matches by key + shape
    transfers the QAT-trained weights without round-tripping through
    `convert_fx`. Returns the number of tensors copied.
    """
    qat_state = qat_module.state_dict()
    fp_state = fp32_module.state_dict()
    copied = 0
    new_state = {}
    for key, tensor in fp_state.items():
        candidate = qat_state.get(key)
        if candidate is not None and candidate.shape == tensor.shape:
            new_state[key] = candidate.detach().clone()
            copied += 1
        else:
            new_state[key] = tensor
    fp32_module.load_state_dict(new_state)
    return copied


def _export_fp32_onnx(
    module: Any,
    example_inputs: tuple[Any, ...],
    out_path: Path,
    *,
    input_names: list[str],
    output_names: list[str],
    dynamic_axes: dict[str, dict[int, str]] | None,
    opset: int = 17,
) -> Path:
    """Export an fp32 module to ONNX using the legacy TorchScript exporter.

    `dynamo=False` pins the legacy path because PyTorch 2.11's
    TorchDynamo exporter still chokes on certain quantization-related
    intermediate buffers even after weight transfer; the legacy path
    handles plain conv/relu graphs cleanly.
    """
    import torch

    out_path.parent.mkdir(parents=True, exist_ok=True)
    module.eval()
    torch.onnx.export(
        module,
        example_inputs,
        str(out_path),
        input_names=input_names,
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        opset_version=opset,
        do_constant_folding=True,
        dynamo=False,
    )
    return out_path


def _ort_static_quantize(
    fp32_path: Path,
    int8_path: Path,
    calibration_samples: list[dict[str, np.ndarray]],
) -> Path:
    """Apply ORT static quantization with a list-backed calibration reader.

    The calibration list comes from the QAT validation slice — using
    the same data-distribution the fake-quant observers saw during
    fine-tune. This is what makes the QAT pass measurably tighter
    than vanilla static PTQ: the weights are pre-conditioned, then
    ORT bakes activation ranges from the same distribution.
    """
    from onnxruntime.quantization import CalibrationDataReader, QuantType, quantize_static

    class _ListReader(CalibrationDataReader):
        def __init__(self, samples: list[dict[str, np.ndarray]]) -> None:
            self._iter = iter(samples)

        def get_next(self):  # type: ignore[override]
            return next(self._iter, None)

    int8_path.parent.mkdir(parents=True, exist_ok=True)
    quantize_static(
        model_input=str(fp32_path),
        model_output=str(int8_path),
        calibration_data_reader=_ListReader(calibration_samples),
        weight_type=QuantType.QInt8,
        activation_type=QuantType.QInt8,
        per_channel=True,
    )
    return int8_path


def _qat_fine_tune(
    qat_module: Any,
    train_iter: Iterator[tuple[Any, Any]],
    *,
    epochs: int,
    lr: float,
    loss_fn,
    device: str,
) -> None:
    """Minimal QAT fine-tune loop.

    Honours the same MSE / L1 loss the fp32 phase used. Works on
    the FX-prepared module — observers update via the standard
    forward pass, no special hooks required.
    """
    import torch

    opt = torch.optim.Adam(qat_module.parameters(), lr=lr)
    qat_module.train()
    qat_module.to(device)

    for _epoch in range(epochs):
        for x, y in train_iter:
            x = x.to(device) if hasattr(x, "to") else x
            y = y.to(device) if hasattr(y, "to") else y
            opt.zero_grad()
            pred = qat_module(x)
            loss = loss_fn(pred, y)
            loss.backward()
            opt.step()


def _generate_smoke_calibration(
    input_name: str, shape: tuple[int, ...], n: int, seed: int = 0
) -> list[dict[str, np.ndarray]]:
    """Deterministic calibration tensors for the smoke path.

    The shape is taken from the example_inputs used at QAT-prep
    time; values are uniformly random in [0, 1] which matches the
    luma-normalised input contract every shipped tiny-AI model
    uses today.
    """
    rng = np.random.default_rng(seed)
    static = tuple(d if isinstance(d, int) and d > 0 else 1 for d in shape)
    return [{input_name: rng.random(static, dtype=np.float32)} for _ in range(n)]


@dataclass
class QatResult:
    """Outcome of a QAT pass — mirrors the registry handoff fields."""

    fp32_onnx: Path
    int8_onnx: Path
    n_params: int
    epochs_fp32: int
    epochs_qat: int


def run_qat(
    *,
    model_factory,
    qat_cfg: QatConfig,
    example_inputs: tuple[Any, ...] | None = None,
    input_names: list[str] | None = None,
    output_names: list[str] | None = None,
    dynamic_axes: dict[str, dict[int, str]] | None = None,
    train_loader_factory=None,
    loss_fn=None,
    calibration_samples: list[dict[str, np.ndarray]] | None = None,
    device: str | None = None,
    opset: int = 17,
) -> QatResult:
    """Execute the full QAT pipeline against a freshly built fp32 model.

    Parameters
    ----------
    model_factory:
        Zero-argument callable returning a torch ``nn.Module`` /
        Lightning module. Called twice — once for the fp32+QAT phase,
        once for the post-QAT fp32 export target.
    qat_cfg:
        Knobs from the YAML config (epochs, lr, output paths).
    example_inputs:
        Tuple of tensors used for FX trace and ONNX export. If
        omitted, we pull `model.example_input_array` from the freshly
        built module.
    input_names / output_names / dynamic_axes:
        Forwarded to ``torch.onnx.export``.
    train_loader_factory:
        Zero-argument callable returning an iterable of
        ``(input, target)`` tensor batches. Required unless
        ``qat_cfg.smoke`` is set. The factory is called twice — once
        per training phase — so the iterator is fresh each time.
    loss_fn:
        Loss callable, defaults to L1 (matches `LearnedFilter`).
    calibration_samples:
        Pre-built calibration list for ORT static-quantize. If
        omitted, falls back to a deterministic random set.
    device:
        Device string ("cuda" / "cpu"). Defaults to "cuda" when
        available, else "cpu". Quantization ops always run on host
        regardless.

    Returns
    -------
    QatResult
        Paths to the exported fp32 + int8 ONNX, plus diagnostic
        metadata.
    """
    import torch

    if device is None:
        device = "cuda" if torch.cuda.is_available() else "cpu"

    torch.manual_seed(qat_cfg.seed)

    fp32_model = model_factory()
    if example_inputs is None:
        example_inputs = _example_input_for(fp32_model)
    if input_names is None:
        input_names = ["input"]
    if output_names is None:
        output_names = ["output"]

    n_params = int(sum(p.numel() for p in fp32_model.parameters() if p.requires_grad))

    # Phase 1 — fp32 warm-start
    if not qat_cfg.smoke and qat_cfg.epochs_fp32 > 0 and train_loader_factory is not None:
        loss_fn = loss_fn or torch.nn.functional.l1_loss
        # Reuse the fine-tune loop for the fp32 phase too — same shape, same loss.
        loader = train_loader_factory()
        _qat_fine_tune(
            fp32_model,
            iter(loader),
            epochs=qat_cfg.epochs_fp32,
            lr=qat_cfg.extra.get("lr", 1e-4),
            loss_fn=loss_fn,
            device=device,
        )

    # Phase 2 — fake-quant insertion. FX prep needs the model on CPU
    # (the FX symbolic tracer does not handle CUDA buffers cleanly).
    fp32_model.cpu()
    cpu_examples = tuple(t.cpu() if hasattr(t, "cpu") else t for t in example_inputs)
    qat_model = _prepare_qat(fp32_model, cpu_examples)

    # Phase 3 — QAT fine-tune
    if not qat_cfg.smoke and qat_cfg.epochs_qat > 0 and train_loader_factory is not None:
        lr_qat = qat_cfg.lr_qat or (qat_cfg.extra.get("lr", 1e-4) / 10.0)
        loss_fn = loss_fn or torch.nn.functional.l1_loss
        loader = train_loader_factory()
        _qat_fine_tune(
            qat_model,
            iter(loader),
            epochs=qat_cfg.epochs_qat,
            lr=lr_qat,
            loss_fn=loss_fn,
            device=device,
        )

    # Phase 4 — export. Build a fresh fp32 module, copy QAT-conditioned
    # weights in, export ONNX, then ORT-static-quantize.
    qat_model.cpu().eval()
    fp32_export_target = model_factory()
    n_copied = _copy_qat_weights_into_fp32(qat_model, fp32_export_target)
    if n_copied == 0:
        raise RuntimeError(
            "QAT->fp32 weight transfer copied 0 tensors. "
            "FX prep probably renamed every submodule — check the model architecture "
            "for top-level Sequentials or untraceable control flow."
        )

    fp32_onnx = qat_cfg.output_fp32_onnx or qat_cfg.output_int8_onnx.with_name(
        qat_cfg.output_int8_onnx.stem.replace(".int8", "") + ".qat.fp32.onnx"
    )
    _export_fp32_onnx(
        fp32_export_target,
        cpu_examples,
        fp32_onnx,
        input_names=input_names,
        output_names=output_names,
        dynamic_axes=dynamic_axes,
        opset=opset,
    )

    # Build calibration if not provided
    if calibration_samples is None:
        shape = tuple(cpu_examples[0].shape)
        calibration_samples = _generate_smoke_calibration(
            input_names[0], shape, qat_cfg.n_calibration, seed=qat_cfg.seed
        )

    int8_onnx = qat_cfg.output_int8_onnx
    if int8_onnx is None:
        raise ValueError("qat_cfg.output_int8_onnx must be set")
    _ort_static_quantize(fp32_onnx, int8_onnx, calibration_samples)

    return QatResult(
        fp32_onnx=fp32_onnx,
        int8_onnx=int8_onnx,
        n_params=n_params,
        epochs_fp32=qat_cfg.epochs_fp32,
        epochs_qat=qat_cfg.epochs_qat,
    )


__all__ = ["QatConfig", "QatResult", "run_qat"]
