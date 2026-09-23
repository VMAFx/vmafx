# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Additional unit tests for compat/python-vmaf/tools/decorator.py — covers
# persist_to_file, persist_to_dir, memoized.__repr__, memoized.__get__,
# memoized with unhashable args, and change_repr.
"""Extended pytest cases for vmaf.tools.decorator."""

from __future__ import annotations

import os


from vmaf.tools.decorator import (
    change_repr,
    memoized,
    persist,
    persist_to_dir,
    persist_to_file,
)

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

    def test_persist_to_file_legacy_sha1_read_through_migration(self, tmp_path):
        """Pre-existing SHA-1 JSON cache entries must be read and promoted to SHA-256 without recomputing."""
        import hashlib
        import json

        cache_file = str(tmp_path / "legacy_migration.json")
        func_name = "cached_worker"
        arg = 42
        raw_key = (func_name + str((arg,))).encode()
        legacy_key = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
        sha256_key = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
        assert len(legacy_key) == 40
        assert len(sha256_key) == 64

        # Pre-populate cache file strictly with legacy SHA-1 key
        with open(cache_file, "wt") as fh:
            json.dump({legacy_key: "legacy_file_val"}, fh)

        call_count = [0]

        @persist_to_file(cache_file)
        def cached_worker(x):
            call_count[0] += 1
            return f"fresh_val_{x}"

        # 1. Read-through hit on legacy entry: function must NOT be called
        val = cached_worker(arg)
        assert val == "legacy_file_val"
        assert call_count[0] == 0

        # Verify promotion: cache_file must now contain both legacy and promoted SHA-256 keys
        with open(cache_file, "rt") as fh:
            persisted = json.load(fh)
        assert persisted[legacy_key] == "legacy_file_val"
        assert persisted[sha256_key] == "legacy_file_val"

        # 2. Subsequent call with new argument: must compute and write ONLY SHA-256 key
        new_arg = 99
        new_raw_key = (func_name + str((new_arg,))).encode()
        new_legacy_key = hashlib.sha1(new_raw_key, usedforsecurity=False).hexdigest()
        new_sha256_key = hashlib.sha256(new_raw_key, usedforsecurity=False).hexdigest()

        val_new = cached_worker(new_arg)
        assert val_new == "fresh_val_99"
        assert call_count[0] == 1

        with open(cache_file, "rt") as fh:
            updated = json.load(fh)
        assert updated[new_sha256_key] == "fresh_val_99"
        assert new_legacy_key not in updated

    def test_persist_to_dir_legacy_sha1_read_through_migration(self, tmp_path):
        """Pre-existing SHA-1 cache files in dir must be read and promoted to SHA-256 without recomputing."""
        import hashlib
        import json

        cache_dir = str(tmp_path / "legacy_dir")
        os.makedirs(cache_dir, exist_ok=True)
        func_name = "dir_worker"
        arg = 123
        raw_key = (func_name + str((arg,))).encode()
        legacy_name = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
        sha256_name = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()
        assert len(legacy_name) == 40
        assert len(sha256_name) == 64

        legacy_path = os.path.join(cache_dir, legacy_name)
        with open(legacy_path, "wt") as fh:
            json.dump("legacy_dir_val", fh)

        call_count = [0]

        @persist_to_dir(cache_dir)
        def dir_worker(x):
            call_count[0] += 1
            return f"fresh_dir_{x}"

        # 1. Read-through hit on legacy file: function must NOT be called
        val = dir_worker(arg)
        assert val == "legacy_dir_val"
        assert call_count[0] == 0

        # Verify promotion: sha256_name file now exists with promoted value
        sha256_path = os.path.join(cache_dir, sha256_name)
        assert os.path.exists(sha256_path)
        with open(sha256_path, "rt") as fh:
            assert json.load(fh) == "legacy_dir_val"

        # 2. Subsequent call with new argument: writes ONLY SHA-256 file, never SHA-1
        new_arg = 456
        new_raw_key = (func_name + str((new_arg,))).encode()
        new_legacy_name = hashlib.sha1(new_raw_key, usedforsecurity=False).hexdigest()
        new_sha256_name = hashlib.sha256(new_raw_key, usedforsecurity=False).hexdigest()

        val_new = dir_worker(new_arg)
        assert val_new == "fresh_dir_456"
        assert call_count[0] == 1

        assert os.path.exists(os.path.join(cache_dir, new_sha256_name))
        assert not os.path.exists(os.path.join(cache_dir, new_legacy_name))

    def test_persist_in_memory_legacy_migration(self):
        """In-memory persist must read preloaded SHA-1 entries and promote them to SHA-256."""
        import hashlib

        call_count = [0]

        @persist
        def mem_func(x):
            call_count[0] += 1
            return x * 10

        # Extract internal closure cache dict
        cache_dict = next(
            c.cell_contents for c in mem_func.__closure__ if isinstance(c.cell_contents, dict)
        )

        func_name = "mem_func"
        arg = 7
        raw_key = (func_name + str((arg,))).encode()
        legacy_key = hashlib.sha1(raw_key, usedforsecurity=False).hexdigest()
        sha256_key = hashlib.sha256(raw_key, usedforsecurity=False).hexdigest()

        # Seed cache dict with legacy SHA-1 entry only
        cache_dict[legacy_key] = 777

        # 1. Calling with arg 7 must return legacy value without invoking mem_func
        val = mem_func(arg)
        assert val == 777
        assert call_count[0] == 0

        # Verify promotion to SHA-256 key
        assert cache_dict[sha256_key] == 777

        # 2. Calling with new arg must invoke function and write ONLY SHA-256 key
        new_arg = 8
        new_raw_key = (func_name + str((new_arg,))).encode()
        new_legacy_key = hashlib.sha1(new_raw_key, usedforsecurity=False).hexdigest()
        new_sha256_key = hashlib.sha256(new_raw_key, usedforsecurity=False).hexdigest()

        val_new = mem_func(new_arg)
        assert val_new == 80
        assert call_count[0] == 1
        assert cache_dict[new_sha256_key] == 80
        assert new_legacy_key not in cache_dict

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
