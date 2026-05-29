# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar/tests/test_replay_buffer.py — unit tests for ReplayBuffer.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

import random
import threading

import pytest

from ai.sidecar.replay_buffer import ReplayBuffer, Sample


class TestReplayBufferBasic:
    def test_empty_on_construction(self):
        buf = ReplayBuffer(capacity=100)
        assert len(buf) == 0

    def test_invalid_capacity_raises(self):
        with pytest.raises(ValueError):
            ReplayBuffer(capacity=0)
        with pytest.raises(ValueError):
            ReplayBuffer(capacity=-1)

    def test_push_and_len(self):
        buf = ReplayBuffer(capacity=5)
        for i in range(3):
            buf.push([float(i)], float(i))
        assert len(buf) == 3

    def test_push_returns_eviction_flag(self):
        buf = ReplayBuffer(capacity=2)
        assert buf.push([1.0], 1.0) is False
        assert buf.push([2.0], 2.0) is False
        # Third push evicts oldest — capacity is full.
        assert buf.push([3.0], 3.0) is True

    def test_fifo_eviction_oldest_dropped(self):
        """The oldest sample is evicted when capacity is exceeded."""
        buf = ReplayBuffer(capacity=3)
        for i in range(5):
            buf.push([float(i)], float(i))
        contents = buf.as_list()
        assert len(contents) == 3
        # Last 3 pushed: 2, 3, 4
        assert [s.true_score for s in contents] == [2.0, 3.0, 4.0]

    def test_as_list_returns_copy(self):
        buf = ReplayBuffer(capacity=10)
        buf.push([1.0], 1.0)
        lst = buf.as_list()
        lst.clear()
        assert len(buf) == 1  # original unaffected

    def test_sample_empty_buffer(self):
        buf = ReplayBuffer(capacity=10)
        assert buf.sample(5) == []

    def test_sample_with_replacement_capped_at_len(self):
        buf = ReplayBuffer(capacity=10)
        buf.push([1.0], 1.0)
        buf.push([2.0], 2.0)
        result = buf.sample(100)
        # sample() caps at len when n > len and no explicit replacement
        assert len(result) == 2

    def test_sample_deterministic_with_seeded_rng(self):
        buf = ReplayBuffer(capacity=20)
        for i in range(10):
            buf.push([float(i)], float(i))
        rng1 = random.Random(42)
        rng2 = random.Random(42)
        assert buf.sample(5, rng=rng1) == buf.sample(5, rng=rng2)

    def test_features_stored_as_tuple(self):
        buf = ReplayBuffer(capacity=5)
        buf.push([1.0, 2.0, 3.0], 50.0)
        s = buf.as_list()[0]
        assert isinstance(s, Sample)
        assert isinstance(s.features, tuple)
        assert s.features == (1.0, 2.0, 3.0)
        assert s.true_score == 50.0


class TestReplayBufferStats:
    def test_stats_dict_keys(self):
        buf = ReplayBuffer(capacity=100)
        stats = buf.stats
        for key in ("capacity", "current_size", "total_pushed", "total_evicted"):
            assert key in stats

    def test_stats_eviction_count(self):
        buf = ReplayBuffer(capacity=3)
        for i in range(7):
            buf.push([float(i)], float(i))
        stats = buf.stats
        assert stats["total_pushed"] == 7
        assert stats["total_evicted"] == 4  # 7 - 3
        assert stats["current_size"] == 3

    def test_repr_contains_capacity(self):
        buf = ReplayBuffer(capacity=42)
        assert "42" in repr(buf)


class TestReplayBufferConcurrency:
    """Stress-test the thread-safety of push/sample."""

    def test_concurrent_push(self):
        buf = ReplayBuffer(capacity=1000)
        errors: list[Exception] = []

        def _push(n: int) -> None:
            try:
                for i in range(n):
                    buf.push([float(i)], float(i))
            except Exception as exc:
                errors.append(exc)

        threads = [threading.Thread(target=_push, args=(200,)) for _ in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors
        # 5 threads × 200 pushes = 1000 total; capacity == 1000 so no eviction.
        assert len(buf) == 1000

    def test_concurrent_push_sample(self):
        buf = ReplayBuffer(capacity=500)
        errors: list[Exception] = []

        def _writer() -> None:
            try:
                for i in range(300):
                    buf.push([float(i)] * 4, float(i % 100))
            except Exception as exc:
                errors.append(exc)

        def _reader() -> None:
            try:
                for _ in range(50):
                    buf.sample(32)
            except Exception as exc:
                errors.append(exc)

        threads = [threading.Thread(target=_writer) for _ in range(3)] + [
            threading.Thread(target=_reader) for _ in range(3)
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert not errors
