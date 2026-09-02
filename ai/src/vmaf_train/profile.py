# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Per-model latency + memory profiling via ORT.

Ships trained tiny models with a latency budget attached, so downstream
consumers (live encode telemetry, edge deployments) know what they're
paying. Runs N warmup iterations followed by M timed iterations on each
available ORT ExecutionProvider; reports mean / p50 / p99 latency and
peak RSS delta.
"""

from __future__ import annotations

import resource
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort

DEFAULT_WARMUP = 5
DEFAULT_ITERS = 100


@dataclass
class ProfileResult:
    provider: str
    shape: tuple[int, ...]
    mean_ms: float
    p50_ms: float
    p99_ms: float
    peak_rss_delta_kb: int
    iters: int


@dataclass
class ProfileReport:
    model_path: Path
    results: list[ProfileResult] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "model": str(self.model_path),
            "results": [r.__dict__ | {"shape": list(r.shape)} for r in self.results],
        }


def _peak_rss_kb() -> int:
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def _infer_input_shape(model_path: Path) -> tuple[int, ...]:
    """Return a concrete shape for the first input, substituting 1 for dyn dims."""
    model = onnx.load(str(model_path))
    if not model.graph.input:
        raise ValueError(f"{model_path} has no graph inputs")
    inp = model.graph.input[0]
    dims: list[int] = []
    for d in inp.type.tensor_type.shape.dim:
        dims.append(d.dim_value if d.dim_value else 1)
    return tuple(dims)


def _available_providers(requested: list[str] | None) -> list[str]:
    all_eps = ort.get_available_providers()
    if requested is None:
        return all_eps
    missing = [p for p in requested if p not in all_eps]
    if missing:
        raise ValueError(f"requested providers {missing} not available; ORT has {all_eps}")
    return requested


def profile_model(
    model_path: Path,
    shapes: list[tuple[int, ...]] | None = None,
    providers: list[str] | None = None,
    warmup: int = DEFAULT_WARMUP,
    iters: int = DEFAULT_ITERS,
    seed: int = 0,
) -> ProfileReport:
    """Profile @p model_path on each (provider, shape) pair."""
    rng = np.random.default_rng(seed)
    report = ProfileReport(model_path=model_path)
    shapes = shapes or [_infer_input_shape(model_path)]
    eps = _available_providers(providers)

    for ep in eps:
        for shape in shapes:
            rss_before = _peak_rss_kb()
            sess = ort.InferenceSession(str(model_path), providers=[ep])
            input_name = sess.get_inputs()[0].name
            x = rng.standard_normal(shape, dtype=np.float32)
            for _ in range(warmup):
                sess.run(None, {input_name: x})
            times = np.empty(iters, dtype=np.float64)
            for i in range(iters):
                t0 = time.perf_counter_ns()
                sess.run(None, {input_name: x})
                times[i] = (time.perf_counter_ns() - t0) / 1e6
            rss_after = _peak_rss_kb()
            report.results.append(
                ProfileResult(
                    provider=ep,
                    shape=tuple(shape),
                    mean_ms=float(times.mean()),
                    p50_ms=float(np.percentile(times, 50)),
                    p99_ms=float(np.percentile(times, 99)),
                    peak_rss_delta_kb=int(max(0, rss_after - rss_before)),
                    iters=iters,
                )
            )
    return report


def render_table(report: ProfileReport) -> str:
    rows = [
        f"{'provider':<28} {'shape':<20} {'mean':>8} {'p50':>8} {'p99':>8} {'Δrss':>10}",
        "-" * 92,
    ]
    for r in report.results:
        shape_str = "×".join(str(s) for s in r.shape)
        rows.append(
            f"{r.provider:<28} {shape_str:<20} "
            f"{r.mean_ms:>7.3f}ms {r.p50_ms:>7.3f}ms {r.p99_ms:>7.3f}ms "
            f"{r.peak_rss_delta_kb:>8}kB"
        )
    return "\n".join(rows)
