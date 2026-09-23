# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Additional unit tests for compat/python-vmaf/tools/decorator.py — covers
# persist_to_file, persist_to_dir, memoized.__repr__, memoized.__get__,
# memoized with unhashable args, and change_repr.
"""Extended pytest cases for vmaf.tools.decorator."""

from __future__ import annotations

import os
import threading

from vmaf.tools import decorator as decorator_module
from vmaf.tools.decorator import (
    change_repr,
    memoized,
    persist,
    persist_to_dir,
    persist_to_file,
)


def _multiprocess_cache_write(cache_file, value):
    """Populate one key from a spawn-safe worker process."""
    import time

    @persist_to_file(cache_file)
    def compute(item):
        time.sleep(0.01)
        return item * 100

    assert compute(value) == value * 100


def _multiprocess_cache_hit(cache_file, value, counter):
    """Count real evaluations from a spawn-safe worker process."""

    @persist_to_file(cache_file)
    def compute(item):
        with counter.get_lock():
            counter.value += 1
        return item * 100

    assert compute(value) == value * 100


# ---------------------------------------------------------------------------
# memoized — additional branches
# ---------------------------------------------------------------------------


class TestMemoizedReprAndGet:
    def test_repr_returns_docstring(self):
        """memoized.__repr__ must return the wrapped function's docstring."""

        @memoized
        def fn_with_doc(x):
            """My docstring."""
            return x

        assert repr(fn_with_doc) == "My docstring."

    def test_repr_none_docstring(self):
        """memoized.__repr__ returns an empty string when the wrapped function has no docstring."""

        @memoized
        def fn_no_doc(x):
            return x

        # repr must return a str, not None, even when func.__doc__ is None.
        result = repr(fn_no_doc)
        assert isinstance(result, str)
        assert result == ""

    def test_unhashable_args_bypass_cache(self):
        """If args are not Hashable, memoized should call the function directly."""

        call_count = [0]

        @memoized
        def fn(items):
            call_count[0] += 1
            return sum(items)

        # Lists are not Hashable — the cache is bypassed and the function is
        # called each time.
        r1 = fn([1, 2, 3])
        r2 = fn([1, 2, 3])
        assert r1 == 6
        assert r2 == 6
        assert call_count[0] == 2  # called twice because args are unhashable

    def test_memoized_as_instance_method(self):
        """memoized.__get__ must return a partial so it works on instance methods."""

        class MyClass:
            @memoized
            def compute(self, x):
                return x * 3

        obj = MyClass()
        assert obj.compute(4) == 12
        assert obj.compute(4) == 12  # second call may use cache


# ---------------------------------------------------------------------------
# persist_to_file
# ---------------------------------------------------------------------------


class TestPersistToFile:
    def test_caches_result_in_file(self, tmp_path):
        cache_file = str(tmp_path / "cache.json")
        call_count = [0]

        @persist_to_file(cache_file)
        def expensive(x):
            call_count[0] += 1
            return x * 7

        r1 = expensive(3)
        assert r1 == 21
        assert call_count[0] == 1
        assert os.path.exists(cache_file)

    def test_reads_cached_value_on_second_call(self, tmp_path):
        cache_file = str(tmp_path / "cache.json")
        call_count = [0]

        @persist_to_file(cache_file)
        def fn(x):
            call_count[0] += 1
            return x + 100

        fn(5)  # populates cache
        fn(5)  # should read from cache
        assert call_count[0] == 1  # only called once

    def test_existing_cache_file_is_loaded(self, tmp_path):
        import hashlib
        import json

        cache_file = str(tmp_path / "preloaded.json")

        # Pre-populate the cache file with a known entry.
        # Key derivation must match persist_to_file's production logic (SHA-256).
        def make_key(fname, args):
            return hashlib.sha256((fname + str(args)).encode(), usedforsecurity=False).hexdigest()

        key = make_key("fn", (42,))
        with open(cache_file, "w") as f:
            json.dump({key: 9999}, f)

        call_count = [0]

        @persist_to_file(cache_file)
        def fn(x):
            call_count[0] += 1
            return x + 1

        result = fn(42)
        assert result == 9999
        assert call_count[0] == 0  # function was NOT called; cache hit


# ---------------------------------------------------------------------------
# persist_to_dir
# ---------------------------------------------------------------------------


class TestPersistToDir:
    def test_caches_result_in_dir(self, tmp_path):
        cache_dir = str(tmp_path / "cache_dir")
        call_count = [0]

        @persist_to_dir(cache_dir)
        def fn(x):
            call_count[0] += 1
            return x * 5

        r = fn(4)
        assert r == 20
        assert call_count[0] == 1
        assert os.path.isdir(cache_dir)

    def test_reads_cached_result_on_second_call(self, tmp_path):
        cache_dir = str(tmp_path / "cache_dir2")
        call_count = [0]

        @persist_to_dir(cache_dir)
        def fn(x):
            call_count[0] += 1
            return x - 10

        fn(20)
        fn(20)
        assert call_count[0] == 1

    def test_different_args_create_separate_cache_entries(self, tmp_path):
        cache_dir = str(tmp_path / "cache_dir3")
        call_count = [0]

        @persist_to_dir(cache_dir)
        def fn(x):
            call_count[0] += 1
            return x * 2

        fn(1)
        fn(2)
        assert call_count[0] == 2
        # Each result is in a separate file in cache_dir.
        assert len(os.listdir(cache_dir)) == 2


# ---------------------------------------------------------------------------
# change_repr
# ---------------------------------------------------------------------------


class TestChangeRepr:
    def test_repr_returns_function_name(self):
        def my_func(x):
            return x + 1

        wrapped = change_repr(my_func)
        assert repr(wrapped) == "my_func"

    def test_call_delegates_to_original(self):
        def adder(a, b):
            return a + b

        wrapped = change_repr(adder)
        assert wrapped(3, 4) == 7

    def test_name_and_doc_preserved(self):
        def documented_func(x):
            """My doc."""
            return x

        wrapped = change_repr(documented_func)
        assert wrapped.__name__ == "documented_func"
        assert wrapped.__doc__ == "My doc."


# ---------------------------------------------------------------------------
# Cache-key stability and collision behavior (SHA-256)
# ---------------------------------------------------------------------------


class TestCacheKeyStabilityAndCollision:
    """Red-capable regression tests for decorator memoization key generation.

    Verifies SHA-256 key generation, 64-hex digest format, stability against
    golden vectors, and collision resistance across function names, argument
    values, argument types, and permutations.
    """

    def test_cache_key_digest_is_sha256_length_and_hex(self, tmp_path):
        import json

        cache_file = str(tmp_path / "sha256_len_check.json")

        @persist_to_file(cache_file)
        def sample(a, b):
            return a + b

        sample(10, 20)
        assert os.path.exists(cache_file)
        with open(cache_file, "r") as fh:
            data = json.load(fh)
        assert len(data) == 1
        key = list(data.keys())[0]
        # SHA-256 digests are strictly 64 hex characters (256 bits).
        # A regression back to SHA-1 (40 chars) will fail this assertion.
        assert len(key) == 64
        assert all(c in "0123456789abcdef" for c in key)

    def test_cache_dir_filename_is_sha256(self, tmp_path):
        cache_dir = str(tmp_path / "sha256_dir_check")

        @persist_to_dir(cache_dir)
        def sample_dir(val):
            return val * 3

        sample_dir("test_val")
        files = os.listdir(cache_dir)
        assert len(files) == 1
        fname = files[0]
        assert len(fname) == 64
        assert all(c in "0123456789abcdef" for c in fname)

    def test_cache_key_stability_golden_vectors(self, tmp_path):
        """Precomputed SHA-256 golden vectors must match exactly across runs."""
        import json

        cache_file = str(tmp_path / "golden.json")

        @persist_to_file(cache_file)
        def golden_func(x, s, pair):
            return 42

        golden_func(1, "alpha", (10, 20))
        with open(cache_file, "r") as fh:
            data = json.load(fh)

        # Golden SHA-256 digest for: "golden_func(1, 'alpha', (10, 20))"
        expected_digest = "2b9706fd8e0323691ef9823c760eeffbd10364f80444db703b2e61a7d95f8429"
        assert expected_digest in data
        assert data[expected_digest] == 42

    def test_cache_key_differentiates_distinct_functions_with_same_args(self, tmp_path):
        """Functions with different names but identical arguments must never collide."""
        cache_dir = str(tmp_path / "func_collision_dir")

        @persist_to_dir(cache_dir)
        def func_alpha(x, y):
            return "alpha"

        @persist_to_dir(cache_dir)
        def func_beta(x, y):
            return "beta"

        res_alpha = func_alpha(1, 2)
        res_beta = func_beta(1, 2)
        assert res_alpha == "alpha"
        assert res_beta == "beta"
        # Must produce two distinct cache files in cache_dir.
        assert len(os.listdir(cache_dir)) == 2

    def test_cache_key_differentiates_argument_permutations_and_types(self, tmp_path):
        """Tuples with reversed order or distinct types must produce distinct keys."""
        cache_dir = str(tmp_path / "type_collision_dir")

        @persist_to_dir(cache_dir)
        def typed_func(arg):
            return str(type(arg))

        # Different argument types and orders
        typed_func(1)
        typed_func("1")
        typed_func(1.0)
        typed_func((1, 2))
        typed_func((2, 1))
        typed_func(())
        typed_func(None)

        files = os.listdir(cache_dir)
        assert len(files) == 7

    def test_persist_to_file_concurrent_threads(self, tmp_path):
        """Concurrent threads must not collide on temp files or corrupt JSON cache."""
        import concurrent.futures
        import json

        cache_file = str(tmp_path / "concurrent_cache.json")
        call_counts = {}
        lock = threading.Lock()

        @persist_to_file(cache_file)
        def compute_val(x):
            with lock:
                call_counts[x] = call_counts.get(x, 0) + 1
            return x * x

        # Run 10 threads concurrently calling compute_val with overlapping and distinct inputs
        inputs = [i % 5 for i in range(50)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            futures = [executor.submit(compute_val, inp) for inp in inputs]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        assert len(results) == 50
        # Verify valid JSON file exists with 5 distinct SHA-256 keys
        assert os.path.exists(cache_file)
        with open(cache_file, "rt", encoding="utf-8") as fh:
            data = json.load(fh)
        assert len(data) == 5
        assert all(len(k) == 64 for k in data.keys())

    def test_persist_to_dir_concurrent_threads(self, tmp_path):
        """Concurrent threads writing to persist_to_dir must use unique temp files without collision."""
        import concurrent.futures
        import json

        cache_dir = str(tmp_path / "concurrent_dir")

        @persist_to_dir(cache_dir)
        def compute_dir(x):
            return f"result_{x}"

        inputs = [i % 5 for i in range(50)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            futures = [executor.submit(compute_dir, inp) for inp in inputs]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        assert len(results) == 50
        files = os.listdir(cache_dir)
        # Exactly 5 files corresponding to inputs 0..4
        assert len(files) == 5
        assert all(len(f) == 64 for f in files)
        for f in files:
            with open(os.path.join(cache_dir, f), "rt", encoding="utf-8") as fh:
                assert json.load(fh).startswith("result_")

    def test_persist_to_file_cold_invalidation_behavior(self, tmp_path):
        """Cold cache misses on legacy files recompute and save under modern SHA-256 keys."""
        import json

        cache_file = str(tmp_path / "legacy_cold.json")
        # Prepopulate with a legacy 40-char key
        legacy_40_char_key = "a" * 40
        with open(cache_file, "wt", encoding="utf-8") as fh:
            json.dump({legacy_40_char_key: "legacy_val"}, fh)

        call_count = [0]

        @persist_to_file(cache_file)
        def func(x):
            call_count[0] += 1
            return x * 10

        # With SHA-1 retired, modern query is a cold miss: computes and persists under 64-char key
        res = func(5)
        assert res == 50
        assert call_count[0] == 1

        with open(cache_file, "rt", encoding="utf-8") as fh:
            data = json.load(fh)
        assert legacy_40_char_key in data
        assert any(len(k) == 64 for k in data.keys())

    def test_persist_in_memory_memoization_stability(self):
        """In-memory persist decorator must memoize deterministically."""
        call_count = [0]

        @persist
        def fib(n):
            call_count[0] += 1
            if n < 2:
                return n
            return fib(n - 1) + fib(n - 2)

        ans = fib(15)
        assert ans == 610
        # Without memoization fib(15) requires 1973 calls.
        # With memoization it evaluates each 0..15 exactly once (16 calls).
        assert call_count[0] == 16

    def test_persist_to_file_multiprocess_concurrent_updates_no_clobber(self, tmp_path):
        """Cross-process concurrent updates must never clobber entries or corrupt the JSON cache."""
        import json
        import multiprocessing as mp

        cache_file = str(tmp_path / "mp_concurrent_cache.json")
        ctx = mp.get_context("spawn")
        procs = [
            ctx.Process(target=_multiprocess_cache_write, args=(cache_file, i)) for i in range(8)
        ]
        for p in procs:
            p.start()
        for p in procs:
            p.join()
            assert p.exitcode == 0

        assert os.path.exists(cache_file)
        with open(cache_file, "rt", encoding="utf-8") as fh:
            data = json.load(fh)

        # Red-capable assertion: without cross-process file locking and disk cache reload/merge,
        # concurrent processes clobber each other's writes and len(data) < 8.
        assert len(data) == 8
        assert all(len(k) == 64 for k in data.keys())

    def test_persist_to_file_cross_process_cache_hit(self, tmp_path):
        """Cross-process invocations must hit disk cache populated by earlier peer processes without recomputation."""
        import multiprocessing as mp

        cache_file = str(tmp_path / "mp_hit_cache.json")
        ctx = mp.get_context("spawn")
        eval_counter = ctx.Value("i", 0)

        # Process 1 computes and writes
        p1 = ctx.Process(target=_multiprocess_cache_hit, args=(cache_file, 99, eval_counter))
        p1.start()
        p1.join()
        assert p1.exitcode == 0
        assert eval_counter.value == 1

        # Process 2 reads and must hit disk cache without re-evaluating
        p2 = ctx.Process(target=_multiprocess_cache_hit, args=(cache_file, 99, eval_counter))
        p2.start()
        p2.join()
        assert p2.exitcode == 0
        assert eval_counter.value == 1

    def test_persist_to_file_reentrant_recursion(self, tmp_path):
        """Recursive functions decorated with persist_to_file must re-enter file locks without deadlocking."""
        cache_file = str(tmp_path / "reentrant_recurse.json")
        eval_counts = [0]

        @persist_to_file(cache_file)
        def fact(n):
            eval_counts[0] += 1
            if n <= 1:
                return 1
            return n * fact(n - 1)

        ans = fact(6)
        assert ans == 720
        assert eval_counts[0] == 6

        import json

        with open(cache_file, "rt", encoding="utf-8") as fh:
            disk_cache = json.load(fh)
        assert len(disk_cache) == 6

        # Second call hits in-memory/disk cache
        ans2 = fact(6)
        assert ans2 == 720
        assert eval_counts[0] == 6

    def test_file_lock_uses_windows_byte_range_backend(self, monkeypatch, tmp_path):
        """The Windows fallback must lock and unlock byte zero through msvcrt."""

        class FakeMsvcrt:
            LK_LOCK = 1
            LK_UNLCK = 2

            def __init__(self):
                self.calls = []

            def locking(self, fd, mode, nbytes):
                self.calls.append((os.lseek(fd, 0, os.SEEK_CUR), mode, nbytes))

        fake_msvcrt = FakeMsvcrt()
        monkeypatch.setattr(decorator_module, "_fcntl", None)
        monkeypatch.setattr(decorator_module, "_msvcrt", fake_msvcrt)

        with decorator_module._file_lock(str(tmp_path / "windows.lock")):
            pass

        assert fake_msvcrt.calls == [(0, fake_msvcrt.LK_LOCK, 1), (0, fake_msvcrt.LK_UNLCK, 1)]
