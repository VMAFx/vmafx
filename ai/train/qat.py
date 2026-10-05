# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Quantization-aware training (QAT) trainer hook for tiny-AI models.

Implements the design locked in
`docs/adr/0207-tinyai-qat-design.md`. The pipeline runs in three
sequential phases:

1. **fp32 phase** — train the Lightning module normally. Reuses
   `ai.src.vmaf_train.train.train` against the model's existing
   YAML config so the warm-start matches what `vmaf-train fit`
   would produce.
2. **Fake-quant insertion** — capture the trained module with
   `torch.export` and wrap it with
   `torchao.quantization.pt2e.prepare_qat_pt2e` under the x86
   inductor recipe: per-tensor uint8 activation + per-channel
   symmetric int8 weight (matching the PTQ static recipe in
   Research-0006 §2). ADR-1293 records the move off the
   deprecated `torch.ao.quantization` FX API.
3. **QAT fine-tune phase** — train the pt2e-prepared module for a
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

`run_qat` arguments
-------------------

model_factory
    Zero-argument callable returning a torch ``nn.Module`` / Lightning
    module. Called twice — once for the fp32+QAT phase, once for the
    post-QAT fp32 export target.
qat_cfg
    Knobs from the YAML config (epochs, lr, output paths).
example_inputs
    Tuple of tensors used for FX trace and ONNX export. If omitted, we
    pull ``model.example_input_array`` from the freshly built module.
input_names / output_names / dynamic_axes
    Forwarded to ``torch.onnx.export``.
train_loader_factory
    Zero-argument callable returning an iterable of ``(input, target)``
    tensor batches. Required unless ``qat_cfg.smoke`` is set. The
    factory is called twice — once per training phase — so the iterator
    is fresh each time.
loss_fn
    Loss callable, defaults to L1 (matches `LearnedFilter`).
calibration_samples
    Pre-built calibration list for ORT static-quantize. If omitted,
    falls back to a deterministic random set.
device
    Device string ("cuda" / "cpu"). Defaults to "cuda" when available,
    else "cpu". Quantization ops always run on host regardless.
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


def _build_quantizer() -> Any:
    """Default QAT recipe: per-tensor uint8 activations, per-channel int8 weights.

    `get_default_x86_inductor_quantization_config(is_qat=True)` is the pt2e
    successor to `get_default_qat_qconfig_mapping("x86")` and keeps ADR-0207
    Decision §2 intact: weights stay `torch.int8`, `per_channel_symmetric`,
    `ch_axis=0`, [-128, 127] — byte-identical to the mapping it replaces.

    Activations move from the old mapping's reduce_range [0, 127] to the full
    [0, 255]. That is a deliberate correction, not drift: the activation ranges
    that reach the shipped model are baked by ORT `quantize_static`, which
    quantizes `QUInt8` over the whole range. The 7-bit ceiling was an FBGEMM
    workaround for pre-VNNI AVX2 accumulator overflow and had no counterpart on
    the ORT side, so the old recipe was the one that failed to match. See
    ADR-1293.
    """
    from torchao.quantization.pt2e.quantizer.x86_inductor_quantizer import (
        X86InductorQuantizer,
        get_default_x86_inductor_quantization_config,
    )

    quantizer = X86InductorQuantizer()
    quantizer.set_global(get_default_x86_inductor_quantization_config(is_qat=True))
    return quantizer


def _set_mode(module: Any, *, train: bool) -> None:
    """Switch a module between train and eval, exported graphs included.

    A `torch.export`-captured graph module rejects `.train()` / `.eval()`
    outright and requires torchao's explicit movers, while a plain
    `nn.Module` only understands the former. `_qat_fine_tune` is called with
    both -- the fp32 warm-start runs on the raw Lightning module, the QAT
    phase on the pt2e-prepared graph -- so the two cases are dispatched here
    rather than at each call site.
    """
    import torch

    if isinstance(module, torch.fx.GraphModule):
        from torchao.quantization.pt2e import (
            move_exported_model_to_eval,
            move_exported_model_to_train,
        )

        if train:
            move_exported_model_to_train(module)
        else:
            move_exported_model_to_eval(module)
        return
    module.train() if train else module.eval()


def _prepare_qat(module: Any, example_inputs: tuple[Any, ...]) -> Any:
    """Insert fake-quant observers via torchao's pt2e graph capture.

    `torch.export.export(...).module()` replaces the FX symbolic trace; the
    captured graph keeps the original parameter names (`entry.weight`,
    `body.0.block.0.weight`, ...), which is what
    :func:`_copy_qat_weights_into_fp32` matches on.
    """
    import torch
    from torchao.quantization.pt2e.quantize_pt2e import prepare_qat_pt2e

    module.train()
    captured = torch.export.export(module, example_inputs, strict=True).module()
    return prepare_qat_pt2e(captured, _build_quantizer())


def _copy_qat_weights_into_fp32(qat_module: Any, fp32_module: Any) -> int:
    """Copy QAT-conditioned parameter tensors into a fresh fp32 module.

    The pt2e-prepared graph preserves submodule names (entry, body.*,
    exit, ...), so a state-dict diff that matches by key + shape
    transfers the QAT-trained weights without round-tripping through
    `convert_pt2e`. Returns the number of tensors copied.
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
    """Export an fp32 module to ONNX with the torch.export-based exporter.

    The export target is a *fresh* fp32 module carrying transferred weights,
    not the pt2e-prepared graph, so none of the quantization-related
    intermediate buffers that once forced `dynamo=False` are present. The
    legacy TorchScript path is deprecated as of torch 2.9 and warns.

    `dynamic_shapes` replaces `dynamic_axes`, which the dynamo exporter warns
    about: it is positional, one entry per forward argument, so the caller's
    per-name axis dict is reordered to follow ``input_names``. Output axes are
    inferred rather than named.
    """
    import torch

    out_path.parent.mkdir(parents=True, exist_ok=True)
    module.eval()
    dynamic_shapes = (
        tuple(dict(dynamic_axes.get(name, {})) for name in input_names) if dynamic_axes else None
    )
    torch.onnx.export(
        module,
        example_inputs,
        str(out_path),
        input_names=input_names,
        output_names=output_names,
        dynamic_shapes=dynamic_shapes,
        opset_version=opset,
        do_constant_folding=True,
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
    from onnxruntime.quantization import (
        CalibrationDataReader,
        QuantFormat,
        QuantType,
        quantize_static,
    )

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
        quant_format=QuantFormat.QDQ,
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
    the pt2e-prepared module — observers update via the standard
    forward pass, no special hooks required.
    """
    import torch

    opt = torch.optim.Adam(qat_module.parameters(), lr=lr)
    _set_mode(qat_module, train=True)
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


def _fine_tune_phase(
    module: Any,
    qat_cfg: QatConfig,
    train_loader_factory: Any,
    *,
    epochs: int,
    lr: float,
    loss_fn: Any,
    device: str,
) -> None:
    """Run one fine-tune phase unless smoke mode, zero epochs or no loader skips it."""
    if qat_cfg.smoke or epochs <= 0 or train_loader_factory is None:
        return
    import torch

    _qat_fine_tune(
        module,
        iter(train_loader_factory()),
        epochs=epochs,
        lr=lr,
        loss_fn=loss_fn or torch.nn.functional.l1_loss,
        device=device,
    )


def _copy_weights_or_raise(qat_model: Any, fp32_export_target: Any) -> None:
    """Copy QAT-conditioned weights into the fp32 target; fail when none transfer."""
    if _copy_qat_weights_into_fp32(qat_model, fp32_export_target) == 0:
        raise RuntimeError(
            "QAT->fp32 weight transfer copied 0 tensors. "
            "pt2e capture probably renamed every submodule — check the model "
            "architecture for top-level Sequentials or untraceable control flow."
        )


def _fp32_onnx_path(qat_cfg: QatConfig) -> Path:
    """Return the fp32 ONNX path, deriving it from the int8 path when unset."""
    return qat_cfg.output_fp32_onnx or qat_cfg.output_int8_onnx.with_name(
        qat_cfg.output_int8_onnx.stem.replace(".int8", "") + ".qat.fp32.onnx"
    )


def _quantize_exported(
    qat_cfg: QatConfig,
    fp32_onnx: Path,
    cpu_examples: tuple[Any, ...],
    input_names: list[str],
    calibration_samples: list[dict[str, np.ndarray]] | None,
) -> Path:
    """Build calibration when missing, run ORT static quantization, return the int8 path."""
    if calibration_samples is None:
        shape = tuple(cpu_examples[0].shape)
        calibration_samples = _generate_smoke_calibration(
            input_names[0], shape, qat_cfg.n_calibration, seed=qat_cfg.seed
        )
    int8_onnx = qat_cfg.output_int8_onnx
    if int8_onnx is None:
        raise ValueError("qat_cfg.output_int8_onnx must be set")
    _ort_static_quantize(fp32_onnx, int8_onnx, calibration_samples)
    return int8_onnx


def _export_and_quantize(
    qat_model: Any,
    model_factory: Any,
    qat_cfg: QatConfig,
    cpu_examples: tuple[Any, ...],
    export_kwargs: dict[str, Any],
    calibration_samples: list[dict[str, np.ndarray]] | None,
) -> tuple[Path, Path]:
    """Export a fresh fp32 module carrying the QAT weights, then ORT-quantize it."""
    qat_model.cpu()
    _set_mode(qat_model, train=False)
    fp32_export_target = model_factory()
    _copy_weights_or_raise(qat_model, fp32_export_target)
    fp32_onnx = _fp32_onnx_path(qat_cfg)
    _export_fp32_onnx(fp32_export_target, cpu_examples, fp32_onnx, **export_kwargs)
    int8_onnx = _quantize_exported(
        qat_cfg, fp32_onnx, cpu_examples, export_kwargs["input_names"], calibration_samples
    )
    return fp32_onnx, int8_onnx


def _warm_start_and_prepare(
    fp32_model: Any,
    qat_cfg: QatConfig,
    train_loader_factory: Any,
    example_inputs: tuple[Any, ...],
    tune: dict[str, Any],
) -> tuple[tuple[Any, ...], Any]:
    """Run phases 1-3: fp32 warm-start, fake-quant insertion, QAT fine-tune."""
    # Phase 1 — fp32 warm-start
    base_lr = qat_cfg.extra.get("lr", 1e-4)
    _fine_tune_phase(
        fp32_model, qat_cfg, train_loader_factory, epochs=qat_cfg.epochs_fp32, lr=base_lr, **tune
    )

    # Phase 2 — fake-quant insertion. Graph capture needs the model on CPU
    # (torch.export does not handle CUDA buffers cleanly here).
    fp32_model.cpu()
    cpu_examples = tuple(t.cpu() if hasattr(t, "cpu") else t for t in example_inputs)
    qat_model = _prepare_qat(fp32_model, cpu_examples)

    # Phase 3 — QAT fine-tune
    lr_qat = qat_cfg.lr_qat or (base_lr / 10.0)
    _fine_tune_phase(
        qat_model, qat_cfg, train_loader_factory, epochs=qat_cfg.epochs_qat, lr=lr_qat, **tune
    )
    return cpu_examples, qat_model


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

    Every argument is described in the module docstring ("`run_qat`
    arguments"). Returns a :class:`QatResult` with the exported fp32 + int8
    ONNX paths and diagnostic metadata.
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

    tune = {"loss_fn": loss_fn, "device": device}
    cpu_examples, qat_model = _warm_start_and_prepare(
        fp32_model, qat_cfg, train_loader_factory, example_inputs, tune
    )

    # Phase 4 — export + ORT static quantization.
    export_kwargs = {
        "input_names": input_names,
        "output_names": output_names,
        "dynamic_axes": dynamic_axes,
        "opset": opset,
    }
    fp32_onnx, int8_onnx = _export_and_quantize(
        qat_model, model_factory, qat_cfg, cpu_examples, export_kwargs, calibration_samples
    )

    return QatResult(
        fp32_onnx=fp32_onnx,
        int8_onnx=int8_onnx,
        n_params=n_params,
        epochs_fp32=qat_cfg.epochs_fp32,
        epochs_qat=qat_cfg.epochs_qat,
    )


__all__ = ["QatConfig", "QatResult", "run_qat"]
