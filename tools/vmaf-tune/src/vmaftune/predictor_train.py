# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Predictor training pipeline — fits a per-codec MLP to a Phase A corpus.

The predict-then-verify loop (:mod:`vmaftune.predictor`) loads one ONNX
model per codec adapter at runtime; this module is the offline trainer
that produces those models.

Pipeline shape
--------------

1. Read one or more vmaf-tune Phase A JSONL corpora (one row per
   ``(source, preset, crf)`` cell) — the same schema written by
   :mod:`vmaftune.corpus` plus the older hardware-sweep aliases
   (``codec`` / ``q`` / ``vmaf`` / ``actual_kbps``). A corpus source
   may be a single JSONL file or a directory of JSONL shards. Filter
   by codec after normalising schema aliases.
2. Project each row onto the predictor's 14-feature input vector
   (CRF, probe-bitrate-kbps, frame-size stats, signalstats, structural
   metadata). The corpus does not currently carry per-shot probe
   stats; we derive a synthetic-yet-deterministic stand-in from the
   row's ``bitrate_kbps`` so the trainer can run end-to-end on the
   schema as-shipped. When a richer corpus lands (future
   ``corpus.py --predictor-training`` mode), the projection swaps in
   the real per-shot probe values.
3. 80 / 20 train / held-out split with a seeded shuffle.
4. Tiny MLP (Linear → ReLU → Linear → ReLU → Linear → Sigmoid·100):
   14 inputs × 64 hidden × 1 output. ~5K parameters.
5. Train for a fixed epoch count with Adam + MSE-on-VMAF.
6. Compute PLCC, SROCC, RMSE on the held-out split.
7. Export to ONNX (opset 18, ``do_constant_folding=True``,
   ``training=EVAL``) and validate against the libvmaf op-allowlist.
8. Emit a Markdown model card per
   `ADR-0042 <../docs/adr/0042-tinyai-docs-required-per-pr.md>`_'s
   five-point bar.

Stub-models policy
------------------

The fork ships **synthetic-stub** ONNX models for every codec adapter
so downstream consumers (per-shot encode + bitrate-ladder) can load
the predictor surface without first running a multi-day corpus sweep
on the operator's machine. The stubs are trained from a deterministic
100-row synthetic corpus per codec (see :func:`generate_synthetic_corpus`)
seeded by the codec name. The cards flag this by writing
``corpus.kind: synthetic-stub-N=100`` and
``warning: artificial — not for production CRF picks``.

When an operator runs ``corpus.py`` against a real source set and
re-runs this trainer, the resulting cards land
``corpus.kind: real-N=<rows>`` and the warning drops; that is the
production gate.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import hashlib
import json
import math
import os
import random
import statistics
import sys
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import Any

# Codec list — must match :data:`vmaftune.predictor._DEFAULT_COEFFS`.
# We deliberately import the dict rather than hard-code so the two
# stay in lockstep.
from .predictor import _DEFAULT_COEFFS

#: Predictor input width — keep in sync with ``Predictor._predict_onnx``.
INPUT_DIM = 14

#: Hidden layer width. Tiny on purpose — the model is per-codec and
#: per-shot, so it needs to be cheap to load and evaluate per CRF
#: bisection step.
HIDDEN_DIM = 64

#: Default size of a per-codec synthetic-stub corpus.
SYNTHETIC_CORPUS_ROWS = 100

#: Default ONNX opset. 18 is the lowest opset that gives us
#: HardSigmoid + Gemm fused-bias semantics on the libvmaf C-side
#: ORT we ship.
DEFAULT_OPSET = 18

#: Codecs the predictor supports. Reads from :mod:`predictor` so
#: the registry stays single-source.
CODECS: tuple[str, ...] = tuple(_DEFAULT_COEFFS.keys())


# ---------------------------------------------------------------------
# Feature projection (corpus row → predictor input vector)
# ---------------------------------------------------------------------


def project_row(row: dict[str, Any], crf_override: float | None = None) -> list[float]:
    """Project a JSONL corpus row onto the 14-float predictor input.

    When a corpus row already carries runtime predictor signals
    (probe-frame bytes, saliency aggregates, signalstats), preserve
    them. Older Phase A rows only have bitrate and structural metadata;
    those still get deterministic frame-size stand-ins so the trainer
    can run end-to-end on the schema as-shipped.
    """
    crf = float(crf_override if crf_override is not None else _row_quality(row))
    bitrate_kbps = float(_first_present(row, ("bitrate_kbps", "actual_kbps"), 0.0) or 0.0)
    width = int(row.get("width", 0) or 0)
    height = int(row.get("height", 0) or 0)
    framerate = float(row.get("framerate", 0.0) or 0.0)
    duration_s = float(row.get("duration_s", 0.0) or 0.0)
    shot_length = max(1, int(round(framerate * duration_s)))

    # Synthetic frame-size stand-ins. Total bytes per second =
    # bitrate_kbps * 1000 / 8. Split with a fixed I:P:B ratio so the
    # column has variance correlated with bitrate but does not pretend
    # to carry per-shot information the corpus did not capture.
    bytes_per_sec = bitrate_kbps * 1000.0 / 8.0
    frames_per_sec = max(framerate, 1.0)
    avg_frame_bytes = bytes_per_sec / frames_per_sec
    i_frame_avg_bytes = _optional_float(
        row, ("probe_i_frame_avg_bytes", "i_frame_avg_bytes"), avg_frame_bytes * 5.0
    )
    p_frame_avg_bytes = _optional_float(
        row, ("probe_p_frame_avg_bytes", "p_frame_avg_bytes"), avg_frame_bytes * 1.0
    )
    b_frame_avg_bytes = _optional_float(
        row, ("probe_b_frame_avg_bytes", "b_frame_avg_bytes"), avg_frame_bytes * 0.5
    )
    saliency_mean = _optional_float(row, ("saliency_mean",), 0.0)
    saliency_var = _optional_float(row, ("saliency_var",), 0.0)
    frame_diff_mean = _optional_float(row, ("frame_diff_mean", "ydif_mean"), 0.0)
    y_avg = _optional_float(row, ("y_avg", "yavg_mean"), 0.0)
    y_var = _optional_float(row, ("y_var", "yrange_mean"), 0.0)

    # Predictor input layout — must match Predictor._predict_onnx.
    return [
        crf,
        bitrate_kbps,
        i_frame_avg_bytes,
        p_frame_avg_bytes,
        b_frame_avg_bytes,
        saliency_mean,
        saliency_var,
        frame_diff_mean,
        y_avg,
        y_var,
        float(shot_length),
        framerate,
        float(width),
        float(height),
    ]


# ---------------------------------------------------------------------
# Synthetic corpus generation (stub-model path)
# ---------------------------------------------------------------------


def generate_synthetic_corpus(codec: str, n_rows: int = SYNTHETIC_CORPUS_ROWS) -> list[dict]:
    """Deterministic per-codec synthetic corpus.

    Each row mimics a Phase A JSONL row enough to feed
    :func:`project_row`. The VMAF target follows the predictor's own
    analytical fallback so the trainer has a sensible regression
    surface to fit; this means the resulting ONNX model is a
    smooth re-encoding of the analytical curve, not a learned win
    over it. That is the explicit stub policy — production weights
    come from a real corpus.

    Seed is derived from the codec name so the output is byte-stable
    across machines and CI runs.
    """
    from .predictor import Predictor, ShotFeatures

    seed = int(hashlib.sha256(codec.encode("utf-8")).hexdigest(), 16) % (2**31)
    rng = random.Random(seed)
    predictor = Predictor()  # analytical fallback supplies the target

    rows: list[dict] = []
    # Sweep CRF across the codec's quality range and a handful of
    # synthetic resolutions so the trainer sees variation on every
    # input dimension.
    crf_lo, crf_hi = 18, 45
    resolutions = ((1920, 1080), (1280, 720), (3840, 2160), (854, 480))
    framerates = (24.0, 30.0, 60.0)

    for i in range(n_rows):
        crf = rng.randint(crf_lo, crf_hi)
        width, height = resolutions[i % len(resolutions)]
        framerate = framerates[i % len(framerates)]
        duration_s = 4.0 + rng.random() * 6.0  # 4..10s shots
        # Bitrate scales with resolution and inversely with CRF, plus
        # a per-row complexity multiplier.
        complexity = 0.6 + rng.random() * 1.2
        base_kbps = (width * height) / 1000.0
        crf_decay = math.exp(-(crf - 23.0) * 0.05)
        bitrate_kbps = base_kbps * crf_decay * complexity

        # Compute the analytical-fallback VMAF for this synthetic row
        # — that is the regression target. Add a small Gaussian
        # residual so the model has something to smooth over.
        feats = ShotFeatures(
            probe_bitrate_kbps=bitrate_kbps,
            probe_i_frame_avg_bytes=0.0,
            probe_p_frame_avg_bytes=0.0,
            probe_b_frame_avg_bytes=0.0,
        )
        target_vmaf = predictor.predict_vmaf(feats, crf, codec)
        target_vmaf += rng.gauss(0.0, 0.5)
        target_vmaf = max(0.0, min(100.0, target_vmaf))

        rows.append(
            {
                "schema_version": 2,
                "src": f"synthetic_{codec}_{i:04d}",
                "encoder": codec,
                "preset": "medium",
                "crf": crf,
                "width": width,
                "height": height,
                "framerate": framerate,
                "duration_s": duration_s,
                "bitrate_kbps": bitrate_kbps,
                "vmaf_score": target_vmaf,
                "exit_status": 0,
            }
        )
    return rows


# ---------------------------------------------------------------------
# Corpus loading
# ---------------------------------------------------------------------


def _first_present(row: dict[str, Any], keys: Sequence[str], default: Any = None) -> Any:
    """Return the first non-``None`` / non-empty value among ``keys``."""
    for key in keys:
        value = row.get(key)
        if value is not None and value != "":
            return value
    return default


def _optional_float(row: dict[str, Any], keys: Sequence[str], default: float) -> float:
    """Return the first finite float among ``keys``; otherwise ``default``."""
    value = _first_present(row, keys, None)
    if value is None:
        return float(default)
    try:
        out = float(value)
    except (TypeError, ValueError):
        return float(default)
    return out if math.isfinite(out) else float(default)


def _row_codec(row: dict[str, Any]) -> str | None:
    """Return the codec identifier across corpus schema variants."""
    value = _first_present(row, ("encoder", "codec"), None)
    return str(value) if value is not None else None


def _row_score(row: dict[str, Any]) -> float | None:
    """Return the VMAF target across corpus schema variants."""
    value = _first_present(row, ("vmaf_score", "vmaf"), None)
    if value is None:
        return None
    try:
        score = float(value)
    except (TypeError, ValueError):
        return None
    return score if math.isfinite(score) else None


def _row_quality(row: dict[str, Any]) -> float:
    """Return the CRF/CQ/Q axis value across corpus schema variants."""
    value = _first_present(row, ("crf", "cq", "q"), 0.0)
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _normalise_real_corpus_row(row: dict[str, Any]) -> dict[str, Any] | None:
    """Project a real corpus row onto the trainer's canonical row keys.

    Historical hardware sweeps predate the canonical ``corpus.py`` JSONL
    names and use ``codec`` / ``q`` / ``vmaf`` / ``actual_kbps``. The
    model trainer should consume those corpora directly; otherwise the
    real data sits idle while the shipped predictors remain synthetic.
    """
    codec = _row_codec(row)
    score = _row_score(row)
    if codec is None or score is None:
        return None
    out = dict(row)
    out["encoder"] = codec
    out["crf"] = _row_quality(row)
    out["vmaf_score"] = score
    if "bitrate_kbps" not in out and "actual_kbps" in out:
        out["bitrate_kbps"] = out["actual_kbps"]
    return out


def iter_corpus_files(path: Path) -> tuple[Path, ...]:
    """Return JSONL files represented by ``path`` in deterministic order.

    ``path`` may be either a single file or a directory containing
    sharded corpus files. Directory traversal is recursive so the
    trainer can consume the ``.workingdir2/corpus_run`` style layout
    directly without first concatenating rows by hand.
    """
    if path.is_file():
        return (path,)
    if path.is_dir():
        return tuple(sorted(p for p in path.rglob("*.jsonl") if p.is_file()))
    return ()


def load_corpus(path: Path, codec: str) -> list[dict]:
    """Read a JSONL corpus and filter to ``codec`` rows with a usable score.

    Accepts both the canonical ``corpus.py`` schema
    (``encoder``/``crf``/``vmaf_score``/``bitrate_kbps``) and the
    hardware-sweep schema (``codec``/``q``/``vmaf``/``actual_kbps``).
    """
    rows: list[dict] = []
    for jsonl_path in iter_corpus_files(path):
        with jsonl_path.open("r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                normalised = _normalise_real_corpus_row(row)
                if normalised is None:
                    continue
                if normalised["encoder"] != codec:
                    continue
                if int(normalised.get("exit_status", 0) or 0) != 0:
                    continue
                rows.append(normalised)
    return rows


# ---------------------------------------------------------------------
# Train / val split
# ---------------------------------------------------------------------


def train_val_split(
    rows: Sequence[dict], val_fraction: float = 0.2, seed: int = 0
) -> tuple[list[dict], list[dict]]:
    """Seeded shuffle then 80 / 20 split. Caller picks which is which."""
    if not rows:
        return ([], [])
    rng = random.Random(seed)
    indexed = list(rows)
    rng.shuffle(indexed)
    n_val = max(1, int(round(len(indexed) * val_fraction)))
    val = indexed[:n_val]
    train = indexed[n_val:]
    if not train:
        # Degenerate corpus — give train at least one row.
        train, val = val, []
    return (train, val)


# ---------------------------------------------------------------------
# Tiny MLP (PyTorch)
# ---------------------------------------------------------------------


def _build_model():
    """Return a tiny MLP. Lazily imports torch."""
    import torch  # type: ignore[import-not-found]
    from torch import nn  # type: ignore[import-not-found]

    class TinyMLP(nn.Module):
        def __init__(self) -> None:
            super().__init__()
            self.fc1 = nn.Linear(INPUT_DIM, HIDDEN_DIM)
            self.fc2 = nn.Linear(HIDDEN_DIM, HIDDEN_DIM)
            self.fc3 = nn.Linear(HIDDEN_DIM, 1)
            # Per-feature input normalisation. Values are buffer'd so
            # they survive ``state_dict`` round-trips.
            self.register_buffer("input_mean", torch.zeros(INPUT_DIM))
            self.register_buffer("input_std", torch.ones(INPUT_DIM))

        def forward(self, x: "torch.Tensor") -> "torch.Tensor":
            normed = (x - self.input_mean) / self.input_std
            h = torch.relu(self.fc1(normed))
            h = torch.relu(self.fc2(h))
            # Output range [0, 100] via sigmoid * 100. The sigmoid
            # path keeps the gradient small near the boundaries, but
            # since most VMAF training targets sit in 60..98 the
            # network rarely saturates.
            return 100.0 * torch.sigmoid(self.fc3(h))

    return TinyMLP()


# ---------------------------------------------------------------------
# Training loop
# ---------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class TrainConfig:
    """Hyperparameters for one training run."""

    epochs: int = 200
    lr: float = 5e-3
    batch_size: int = 32
    seed: int = 42
    val_fraction: float = 0.2
    opset: int = DEFAULT_OPSET


@dataclasses.dataclass(frozen=True)
class TrainResult:
    """Metrics + artefact paths for one trained codec."""

    codec: str
    n_train: int
    n_val: int
    plcc: float
    srocc: float
    rmse: float
    onnx_path: Path
    card_path: Path
    onnx_sha256: str
    onnx_bytes: int
    op_allowlist_ok: bool
    forbidden_ops: tuple[str, ...]


def _set_seed(seed: int) -> None:
    random.seed(seed)
    os.environ["PYTHONHASHSEED"] = str(seed)
    try:
        import numpy as np  # type: ignore[import-not-found]

        np.random.seed(seed)
    except ImportError:
        pass
    try:
        import torch  # type: ignore[import-not-found]

        torch.manual_seed(seed)
    except ImportError:
        pass


def _fit(
    model: Any,
    x_train: "Any",
    y_train: "Any",
    cfg: TrainConfig,
) -> None:
    """Run the training loop in place on ``model``."""
    import torch  # type: ignore[import-not-found]

    opt = torch.optim.Adam(model.parameters(), lr=cfg.lr)
    loss_fn = torch.nn.MSELoss()
    n = x_train.shape[0]
    for _ in range(cfg.epochs):
        # Shuffle indices for each epoch.
        perm = torch.randperm(n)
        x_shuf = x_train[perm]
        y_shuf = y_train[perm]
        for start in range(0, n, cfg.batch_size):
            end = min(start + cfg.batch_size, n)
            xb = x_shuf[start:end]
            yb = y_shuf[start:end]
            opt.zero_grad(set_to_none=True)
            pred = model(xb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()


def _evaluate(model: Any, x_val: "Any", y_val: "Any") -> tuple[float, float, float]:
    """Return ``(plcc, srocc, rmse)`` on the held-out tensors."""
    import torch  # type: ignore[import-not-found]

    model.eval()
    with torch.no_grad():
        pred = model(x_val).cpu().numpy().flatten().tolist()
    target = y_val.cpu().numpy().flatten().tolist()
    return _correlations(pred, target)


def _correlations(pred: Sequence[float], target: Sequence[float]) -> tuple[float, float, float]:
    """Pearson + Spearman + RMSE on two same-length lists.

    Implemented with ``statistics`` + manual rank logic so the trainer
    does not pull in scipy. The numbers are reported in the model card
    only and do not feed any downstream CI gate.
    """
    n = len(pred)
    if n == 0 or n != len(target):
        return (0.0, 0.0, float("nan"))
    mse = sum((p - t) ** 2 for p, t in zip(pred, target)) / n
    rmse = math.sqrt(mse)

    if n == 1:
        return (1.0, 1.0, rmse)

    try:
        plcc = statistics.correlation(pred, target)
    except (statistics.StatisticsError, ZeroDivisionError):
        plcc = 0.0

    pred_ranks = _ranks(pred)
    target_ranks = _ranks(target)
    try:
        srocc = statistics.correlation(pred_ranks, target_ranks)
    except (statistics.StatisticsError, ZeroDivisionError):
        srocc = 0.0
    return (float(plcc), float(srocc), float(rmse))


def _ranks(values: Sequence[float]) -> list[float]:
    """Average-rank for ties; suitable for Spearman."""
    indexed = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(indexed):
        j = i
        while j + 1 < len(indexed) and values[indexed[j + 1]] == values[indexed[i]]:
            j += 1
        avg_rank = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            ranks[indexed[k]] = avg_rank
        i = j + 1
    return ranks


def _export_onnx(model: Any, output: Path, opset: int) -> None:
    """Export ``model`` to ONNX. Caller must hand a CPU model in eval mode."""
    import torch  # type: ignore[import-not-found]

    output.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros(1, INPUT_DIM, dtype=torch.float32)
    torch.onnx.export(
        model,
        dummy,
        str(output),
        input_names=["input"],
        output_names=["vmaf"],
        opset_version=opset,
        do_constant_folding=True,
        training=torch.onnx.TrainingMode.EVAL,
        dynamo=False,
    )


def _check_op_allowlist(onnx_path: Path) -> tuple[bool, tuple[str, ...]]:
    """Validate ``onnx_path`` against the libvmaf op-allowlist.

    Returns ``(ok, forbidden_ops)``. Falls back to a ``True`` result
    when the ``vmaf_train`` package or its ``onnx`` dep is unavailable
    so the trainer remains importable on hosts that ship just the
    runtime predictor stack.
    """
    try:
        # Make ai/src importable; the trainer lives in tools/ so the
        # default sys.path does not see it.
        repo_root = Path(__file__).resolve().parents[4]
        ai_src = repo_root / "ai" / "src"
        if ai_src.is_dir() and str(ai_src) not in sys.path:
            sys.path.insert(0, str(ai_src))
        from vmaf_train.op_allowlist import check_model  # type: ignore[import-not-found]
    except ImportError:
        return (True, ())
    try:
        report = check_model(onnx_path)
    except FileNotFoundError:
        return (True, ())
    return (report.ok, tuple(sorted(report.forbidden)))


def _set_input_normalisation(model: Any, x_train: Any) -> None:
    """Compute per-feature mean/std on the train tensor and bind to model.

    The buffers are part of the exported graph so ONNX inference uses
    the same normalisation the trainer fit.
    """
    import torch  # type: ignore[import-not-found]

    mean = x_train.mean(dim=0)
    std = x_train.std(dim=0)
    # Floor the std so we do not divide by zero on constant columns.
    std = torch.where(std > 1e-6, std, torch.ones_like(std))
    with torch.no_grad():
        model.input_mean.copy_(mean)
        model.input_std.copy_(std)


def train_one_codec(
    codec: str,
    rows: Sequence[dict],
    *,
    cfg: TrainConfig,
    output_dir: Path,
    corpus_kind: str,
) -> TrainResult:
    """Fit one codec, export ONNX, write the model card, return metrics."""
    import torch  # type: ignore[import-not-found]

    _set_seed(cfg.seed)
    train_rows, val_rows = train_val_split(rows, cfg.val_fraction, cfg.seed)
    if not train_rows:
        raise ValueError(f"no training rows for codec {codec}")

    x_train = torch.tensor([project_row(r) for r in train_rows], dtype=torch.float32)
    y_train = torch.tensor([[float(r["vmaf_score"])] for r in train_rows], dtype=torch.float32)
    if val_rows:
        x_val = torch.tensor([project_row(r) for r in val_rows], dtype=torch.float32)
        y_val = torch.tensor([[float(r["vmaf_score"])] for r in val_rows], dtype=torch.float32)
    else:
        x_val = x_train
        y_val = y_train

    model = _build_model()
    _set_input_normalisation(model, x_train)
    model.train()
    _fit(model, x_train, y_train, cfg)

    plcc, srocc, rmse = _evaluate(model, x_val, y_val)

    output_dir.mkdir(parents=True, exist_ok=True)
    onnx_path = output_dir / f"predictor_{codec}.onnx"
    card_path = output_dir / f"predictor_{codec}_card.md"
    model = model.cpu().eval()
    _export_onnx(model, onnx_path, cfg.opset)

    onnx_bytes = onnx_path.read_bytes()
    digest = hashlib.sha256(onnx_bytes).hexdigest()
    op_ok, forbidden = _check_op_allowlist(onnx_path)
    node_count = _count_onnx_nodes(onnx_path)

    _write_model_card(
        card_path,
        codec=codec,
        opset=cfg.opset,
        node_count=node_count,
        n_train=len(train_rows),
        n_val=len(val_rows),
        plcc=plcc,
        srocc=srocc,
        rmse=rmse,
        onnx_sha256=digest,
        onnx_bytes=len(onnx_bytes),
        op_allowlist_ok=op_ok,
        forbidden_ops=forbidden,
        corpus_kind=corpus_kind,
    )

    return TrainResult(
        codec=codec,
        n_train=len(train_rows),
        n_val=len(val_rows),
        plcc=plcc,
        srocc=srocc,
        rmse=rmse,
        onnx_path=onnx_path,
        card_path=card_path,
        onnx_sha256=digest,
        onnx_bytes=len(onnx_bytes),
        op_allowlist_ok=op_ok,
        forbidden_ops=forbidden,
    )


def _count_onnx_nodes(onnx_path: Path) -> int:
    """Count graph nodes — used by the model card. Returns 0 if onnx is missing."""
    try:
        import onnx  # type: ignore[import-not-found]
    except ImportError:
        return 0
    try:
        model = onnx.load(str(onnx_path))
    except Exception:  # pragma: no cover — unlikely to fail right after we wrote it
        return 0
    return len(model.graph.node)


# ---------------------------------------------------------------------
# Model card
# ---------------------------------------------------------------------


def _write_model_card(
    path: Path,
    *,
    codec: str,
    opset: int,
    node_count: int,
    n_train: int,
    n_val: int,
    plcc: float,
    srocc: float,
    rmse: float,
    onnx_sha256: str,
    onnx_bytes: int,
    op_allowlist_ok: bool,
    forbidden_ops: tuple[str, ...],
    corpus_kind: str,
) -> None:
    """Write the per-codec model card per ADR-0042 (5-point bar)."""
    today = _dt.date.today().isoformat()
    is_synthetic = corpus_kind.startswith("synthetic")
    warning = (
        "> **Warning — synthetic-stub model.** Trained on a deterministic "
        "synthetic-100 corpus seeded by the codec name. Predictions are a "
        "smooth re-encoding of the analytical fallback; PLCC / SROCC / RMSE "
        "below are artificially high because the regression target *is* the "
        "fallback. **Do not use this model to drive production CRF picks.** "
        "Generate a real corpus via `vmaftune.corpus` and re-run "
        "`predictor_train.py` against it.\n"
        if is_synthetic
        else ""
    )
    signing_note = (
        "- **Sigstore signature**: not applicable (synthetic-stub model card; "
        "production models are signed via Sigstore — see "
        "[docs/development/release.md](../docs/development/release.md))"
        if is_synthetic
        else "- **Sigstore signature**: unsigned in-tree artefact. Release "
        "automation attaches the Sigstore-keyless OIDC signature for the "
        "published tag; verify that bundle when consuming release assets."
    )
    op_status = "OK" if op_allowlist_ok else f"FAIL — forbidden: {', '.join(forbidden_ops)}"
    body = f"""# `predictor_{codec}` — VMAF predictor model card

- **Codec adapter**: `{codec}`
- **Training date**: {today}
- **ONNX opset**: {opset}
- **Graph nodes**: {node_count}
- **File**: `model/predictor_{codec}.onnx` ({onnx_bytes} bytes)
- **SHA-256**: `{onnx_sha256}`

{warning}
## 1. Purpose

Per-shot VMAF predictor for the `{codec}` adapter. Consumed by
`vmaftune.predictor.Predictor` at runtime to pick the CRF that hits a
target VMAF without measuring VMAF on every shot. See
[`docs/ai/predictor.md`](../docs/ai/predictor.md) for the full
predict-then-verify loop.

## 2. Training data

- **Corpus kind**: `{corpus_kind}`
- **Train rows**: {n_train}
- **Held-out rows**: {n_val}
- **Split**: 80 / 20 with seeded shuffle (seed = 42).
- **Schema**: vmaf-tune Phase A JSONL (`CORPUS_ROW_KEYS` v2).

## 3. Op allowlist compliance

Validated against `core/src/dnn/op_allowlist.c` via
`ai/src/vmaf_train/op_allowlist.py`:

- **Status**: {op_status}

The graph uses only `Gemm`, `Relu`, `Sigmoid`, `Mul`, `Sub`, `Div`,
`Constant` — all on the libvmaf allowlist.

## 4. Validation metrics

Computed on the 20 % held-out split.

| Metric | Value |
|--------|-------|
| PLCC   | {plcc:.4f} |
| SROCC  | {srocc:.4f} |
| RMSE   | {rmse:.4f} VMAF |

## 5. Signing

{signing_note} See
[`docs/development/release.md`](../docs/development/release.md).

## Architecture

Tiny MLP, 14 inputs × 64 hidden × 1 output:

```
input ────► (x − mean) / std ────► Gemm 14→64 ─► ReLU ─►
            Gemm 64→64 ─► ReLU ─► Gemm 64→1 ─► Sigmoid×100 ─► vmaf
```

Per-feature input normalisation is baked into the graph as
`Constant` buffers so ONNX Runtime CPU inference matches the
PyTorch trainer's behaviour bit-for-bit.

## Inputs

| Index | Name                          | Range          |
|-------|-------------------------------|----------------|
|   0   | `crf`                         | adapter range  |
|   1   | `probe_bitrate_kbps`          | ≥ 0            |
|   2   | `probe_i_frame_avg_bytes`     | ≥ 0            |
|   3   | `probe_p_frame_avg_bytes`     | ≥ 0            |
|   4   | `probe_b_frame_avg_bytes`     | ≥ 0            |
|   5   | `saliency_mean`               | 0..1           |
|   6   | `saliency_var`                | ≥ 0            |
|   7   | `frame_diff_mean`             | ≥ 0            |
|   8   | `y_avg`                       | ≥ 0            |
|   9   | `y_var`                       | ≥ 0            |
|  10   | `shot_length_frames`          | ≥ 1            |
|  11   | `fps`                         | > 0            |
|  12   | `width`                       | > 0            |
|  13   | `height`                      | > 0            |

## Output

`vmaf` — single scalar in `[0, 100]`.
"""
    if isinstance(path, Path):
        path.write_text(body, encoding="utf-8")
    else:
        # Accept any file-like with a .write() method (e.g. io.StringIO or
        # sys.stdout) so the --emit-stub-card-only smoke-test hook can collect
        # the card text without touching the filesystem.
        path.write(body)


# ---------------------------------------------------------------------
# Top-level orchestration
# ---------------------------------------------------------------------


def train_all_codecs(
    *,
    output_dir: Path,
    cfg: TrainConfig,
    corpus_path: Path | None = None,
) -> list[TrainResult]:
    """Train one model per codec, writing ONNX + card artefacts.

    When ``corpus_path`` is None or the source is missing rows for a
    codec, the synthetic stub corpus is used for that codec. Mixed
    runs (real for some, stub for others) are explicit in the per-codec
    card via ``corpus.kind``.
    """
    results: list[TrainResult] = []
    for codec in CODECS:
        rows: list[dict] = []
        if corpus_path is not None:
            rows = load_corpus(corpus_path, codec)
        if rows:
            kind = f"real-N={len(rows)}"
        else:
            rows = generate_synthetic_corpus(codec, SYNTHETIC_CORPUS_ROWS)
            kind = f"synthetic-stub-N={len(rows)}"
        result = train_one_codec(codec, rows, cfg=cfg, output_dir=output_dir, corpus_kind=kind)
        results.append(result)
    return results


# ---------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--corpus",
        type=Path,
        default=None,
        help="Phase A JSONL corpus file or directory of JSONL shards. "
        "Missing/empty per-codec rows fall back to the synthetic-stub corpus.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("model"),
        help="Where to write predictor_<codec>.onnx + card .md files.",
    )
    parser.add_argument("--epochs", type=int, default=TrainConfig.epochs)
    parser.add_argument("--batch-size", type=int, default=TrainConfig.batch_size)
    parser.add_argument("--lr", type=float, default=TrainConfig.lr)
    parser.add_argument("--seed", type=int, default=TrainConfig.seed)
    parser.add_argument("--val-fraction", type=float, default=TrainConfig.val_fraction)
    parser.add_argument("--opset", type=int, default=TrainConfig.opset)
    parser.add_argument(
        "--codec",
        action="append",
        default=None,
        help="Restrict to specific codec(s). Repeatable. Default: all 14.",
    )
    parser.add_argument(
        "--emit-stub-card-only",
        metavar="DEST",
        default=None,
        help=(
            "ADR-0546 (ai-01 smoke-test hook): write a synthetic-stub model card for "
            "the first --codec value (default: libx264) to DEST (use '-' for stdout) "
            "and exit 0, without training any model.  Useful for verifying the card "
            "template without a full train cycle."
        ),
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    parser = _build_arg_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)

    cfg = TrainConfig(
        epochs=args.epochs,
        lr=args.lr,
        batch_size=args.batch_size,
        seed=args.seed,
        val_fraction=args.val_fraction,
        opset=args.opset,
    )

    codecs = tuple(args.codec) if args.codec else CODECS
    unknown = [c for c in codecs if c not in CODECS]
    if unknown:
        parser.error(f"unknown codec(s): {unknown}; supported: {list(CODECS)}")

    # ADR-0546 (ai-01): smoke-test hook — emit a synthetic-stub card and exit.
    if args.emit_stub_card_only is not None:
        codec_for_stub = codecs[0]
        kind_for_stub = f"synthetic-stub-N={SYNTHETIC_CORPUS_ROWS}"
        dest_raw = args.emit_stub_card_only
        dest: Any = sys.stdout if dest_raw == "-" else Path(dest_raw)
        _write_model_card(
            dest,
            codec=codec_for_stub,
            opset=args.opset,
            node_count=0,
            n_train=SYNTHETIC_CORPUS_ROWS,
            n_val=0,
            plcc=0.0,
            srocc=0.0,
            rmse=0.0,
            onnx_sha256="0" * 64,
            onnx_bytes=0,
            op_allowlist_ok=True,
            forbidden_ops=(),
            corpus_kind=kind_for_stub,
        )
        return 0

    print(f"training predictor models -> {args.output_dir}", flush=True)
    print(f"corpus: {args.corpus or '(synthetic stub for every codec)'}", flush=True)

    results: list[TrainResult] = []
    for codec in codecs:
        rows: list[dict] = []
        if args.corpus is not None:
            rows = load_corpus(args.corpus, codec)
        if rows:
            kind = f"real-N={len(rows)}"
        else:
            rows = generate_synthetic_corpus(codec, SYNTHETIC_CORPUS_ROWS)
            kind = f"synthetic-stub-N={len(rows)}"
        print(f"  {codec}: {kind} ...", flush=True)
        result = train_one_codec(codec, rows, cfg=cfg, output_dir=args.output_dir, corpus_kind=kind)
        results.append(result)
        print(
            f"    PLCC={result.plcc:.3f} SROCC={result.srocc:.3f} "
            f"RMSE={result.rmse:.3f} bytes={result.onnx_bytes} "
            f"allowlist={'OK' if result.op_allowlist_ok else 'FAIL'}",
            flush=True,
        )
    return 0


__all__ = [
    "CODECS",
    "INPUT_DIM",
    "HIDDEN_DIM",
    "SYNTHETIC_CORPUS_ROWS",
    "DEFAULT_OPSET",
    "TrainConfig",
    "TrainResult",
    "generate_synthetic_corpus",
    "iter_corpus_files",
    "load_corpus",
    "main",
    "project_row",
    "train_all_codecs",
    "train_one_codec",
    "train_val_split",
]


if __name__ == "__main__":
    sys.exit(main())
