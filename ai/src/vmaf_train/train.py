# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Main training entry, driven by a YAML config or direct kwargs."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

import pytorch_lightning as L
import yaml
from pydantic import BaseModel, ConfigDict, Field, field_validator
from pytorch_lightning.callbacks import ModelCheckpoint

from .datamodule import VmafTrainDataModule
from .models import FRRegressor, LearnedFilter, NRMetric

MODEL_REGISTRY: dict[str, type[L.LightningModule]] = {
    "fr_regressor": FRRegressor,
    "nr_metric": NRMetric,
    "learned_filter": LearnedFilter,
}

# Subset of the Literal union accepted by pytorch_lightning.Trainer(precision=...).
# Only the string forms actually used in this repo are listed; extend as needed.
# Also imported by scripts that call Trainer directly (e.g. train_konvid.py).
PrecisionStr = Literal[
    "transformer-engine",
    "transformer-engine-float16",
    "16-true",
    "16-mixed",
    "bf16-true",
    "bf16-mixed",
    "32-true",
    "64-true",
    "64",
    "32",
    "16",
    "bf16",
]


class TrainConfig(BaseModel):
    """User-supplied training configuration (YAML / kwargs).

    Migrated from ``@dataclass`` to ``pydantic.BaseModel`` so that loader
    sites (``load_config``) surface declarative, line-numbered validation
    errors instead of silently coercing or accepting bad inputs. See
    ADR-0934.
    """

    model_config = ConfigDict(
        # ``arbitrary_types_allowed`` keeps ``pathlib.Path`` round-tripping
        # without forcing a converter; pydantic v2's native ``Path`` support
        # already coerces from string but we keep the field type explicit.
        extra="forbid",
        validate_assignment=True,
    )

    model: str
    model_args: dict[str, Any] = Field(default_factory=dict)
    cache: Path
    output: Path = Path("runs/default")
    epochs: int = 50
    batch_size: int = 256
    val_frac: float = 0.1
    test_frac: float = 0.1
    seed: int = 0
    precision: PrecisionStr = "32-true"

    @field_validator("epochs", "batch_size")
    @classmethod
    def _positive_int(cls, v: int) -> int:
        if v <= 0:
            raise ValueError("must be > 0")
        return v

    @field_validator("val_frac", "test_frac")
    @classmethod
    def _unit_interval(cls, v: float) -> float:
        if not 0.0 <= v < 1.0:
            raise ValueError("must lie in [0.0, 1.0)")
        return v

    @field_validator("seed")
    @classmethod
    def _nonnegative(cls, v: int) -> int:
        if v < 0:
            raise ValueError("must be >= 0")
        return v


def load_config(path: Path, overrides: dict[str, Any] | None = None) -> TrainConfig:
    with path.open() as fh:
        doc = yaml.safe_load(fh) or {}
    if overrides:
        doc.update({k: v for k, v in overrides.items() if v is not None})
    return TrainConfig.model_validate(doc)


def train(cfg: TrainConfig) -> Path:
    if cfg.model not in MODEL_REGISTRY:
        raise KeyError(f"unknown model kind: {cfg.model}")
    L.seed_everything(cfg.seed, workers=True)
    model_cls = MODEL_REGISTRY[cfg.model]
    model = model_cls(**cfg.model_args)

    dm = VmafTrainDataModule(
        cfg.cache,
        batch_size=cfg.batch_size,
        val_frac=cfg.val_frac,
        test_frac=cfg.test_frac,
    )

    cfg.output.mkdir(parents=True, exist_ok=True)
    ckpt_cb = ModelCheckpoint(
        dirpath=cfg.output,
        filename="best",
        monitor="val/mse" if cfg.model != "learned_filter" else "val/l1",
        mode="min",
        save_top_k=1,
        save_last=True,
    )
    trainer = L.Trainer(
        max_epochs=cfg.epochs,
        callbacks=[ckpt_cb],
        default_root_dir=cfg.output,
        log_every_n_steps=10,
        precision=cfg.precision,
        deterministic=True,
    )
    trainer.fit(model, datamodule=dm)
    return cfg.output / "last.ckpt"
