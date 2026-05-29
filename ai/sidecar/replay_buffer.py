# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar/replay_buffer.py — Bounded ring buffer for online training replay.
#
# Implements a thread-safe, fixed-capacity FIFO ring buffer that stores
# (features, true_score) samples for replay during online SGD training.
# When capacity is reached the oldest sample is evicted (FIFO, not random).
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from __future__ import annotations

import random as _random_mod
import threading
from collections import deque
from typing import TYPE_CHECKING, NamedTuple, Sequence

if TYPE_CHECKING:
    import random


class Sample(NamedTuple):
    """A single training sample."""

    features: tuple[float, ...]  # immutable feature vector
    true_score: float  # ground-truth VMAF score (0–100)


class ReplayBuffer:
    """Thread-safe bounded ring buffer for experience replay.

    Stores up to *capacity* ``Sample`` objects.  When full, the oldest
    entry is silently evicted (FIFO).  Thread safety is provided by a
    single ``threading.Lock``; contention is negligible for the expected
    write rate (< 100 samples/s) and batch sizes (32–256 samples).

    Parameters
    ----------
    capacity:
        Maximum number of samples to retain.  Default 10 000.
        ADR-0781 §Replay buffer: chosen so that a 50-encode-per-hour
        workload retains ~200 hours of history — enough to represent
        diverse content without unbounded memory growth (~40 MB at
        80 float32 features per sample).
    """

    def __init__(self, capacity: int = 10_000) -> None:
        if capacity <= 0:
            raise ValueError(f"capacity must be positive, got {capacity!r}")
        self._capacity = capacity
        self._buf: deque[Sample] = deque(maxlen=capacity)
        self._lock = threading.Lock()
        self._total_pushed: int = 0  # monotonic push counter (never wraps in Python)
        self._total_evicted: int = 0

    # ------------------------------------------------------------------
    # Write path
    # ------------------------------------------------------------------

    def push(self, features: Sequence[float], true_score: float) -> bool:
        """Append a sample.  Returns True if an eviction occurred.

        The eviction is silent: the caller is informed via the return
        value so the Go-side counter ``vmafx_training_replay_evictions_total``
        can be incremented without polling ``evicted`` separately.
        """
        sample = Sample(tuple(features), float(true_score))
        with self._lock:
            eviction = len(self._buf) == self._capacity
            self._buf.append(sample)  # deque(maxlen=N) auto-evicts left
            self._total_pushed += 1
            if eviction:
                self._total_evicted += 1
        return eviction

    # ------------------------------------------------------------------
    # Read path
    # ------------------------------------------------------------------

    def sample(self, n: int, rng: "random.Random | None" = None) -> list[Sample]:
        """Draw *n* samples (with replacement when n > len(buffer)).

        When *n* exceeds the current buffer size all available samples
        are returned (no replacement).  Pass a seeded ``random.Random``
        for deterministic unit-tests.
        """
        r = rng or _random_mod
        with self._lock:
            snapshot = list(self._buf)
        if not snapshot:
            return []
        n = min(n, len(snapshot))
        return r.choices(snapshot, k=n)

    def as_list(self) -> list[Sample]:
        """Return a shallow copy of all samples in insertion order."""
        with self._lock:
            return list(self._buf)

    # ------------------------------------------------------------------
    # Inspection / metrics
    # ------------------------------------------------------------------

    @property
    def capacity(self) -> int:
        return self._capacity

    def __len__(self) -> int:
        with self._lock:
            return len(self._buf)

    def __repr__(self) -> str:
        with self._lock:
            return (
                f"ReplayBuffer(capacity={self._capacity}, "
                f"current={len(self._buf)}, "
                f"total_pushed={self._total_pushed}, "
                f"total_evicted={self._total_evicted})"
            )

    @property
    def stats(self) -> dict[str, int]:
        """Prometheus-friendly counters snapshot."""
        with self._lock:
            return {
                "capacity": self._capacity,
                "current_size": len(self._buf),
                "total_pushed": self._total_pushed,
                "total_evicted": self._total_evicted,
            }
