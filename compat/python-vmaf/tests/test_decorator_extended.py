# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Additional unit tests for compat/python-vmaf/tools/decorator.py — covers
# persist_to_file, persist_to_dir, memoized.__repr__, memoized.__get__,
# memoized with unhashable args, and change_repr.
"""Extended pytest cases for vmaf.tools.decorator."""

from __future__ import annotations

from importlib import import_module
from pathlib import Path

from vmaf.tools.decorator import change_repr, memoized, persist_to_dir, persist_to_file

_COMPARISON_VALUE_12 = 12
_COMPARISON_VALUE_2 = 2
_COMPARISON_VALUE_20 = 20
_COMPARISON_VALUE_21 = 21
_COMPARISON_VALUE_6 = 6
_COMPARISON_VALUE_7 = 7
_COMPARISON_VALUE_9999 = 9999

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
        assert r1 == _COMPARISON_VALUE_6
        assert r2 == _COMPARISON_VALUE_6
        assert call_count[0] == _COMPARISON_VALUE_2  # called twice because args are unhashable

    def test_memoized_as_instance_method(self):
        """memoized.__get__ must return a partial so it works on instance methods."""

        class MyClass:
            @memoized
            def compute(self, x):
                return x * 3

        obj = MyClass()
        assert obj.compute(4) == _COMPARISON_VALUE_12
        assert obj.compute(4) == _COMPARISON_VALUE_12  # second call may use cache


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
        assert r1 == _COMPARISON_VALUE_21
        assert call_count[0] == 1
        assert Path(cache_file).exists()

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
        hashlib = import_module("hashlib")
        json = import_module("json")

        cache_file = str(tmp_path / "preloaded.json")

        # Pre-populate the cache file with a known entry.
        # Key derivation must match persist_to_file's production logic.
        # SHA-1 is used here only as a memoization cache key, not for security.
        def make_key(fname, args):
            return hashlib.sha1((fname + str(args)).encode(), usedforsecurity=False).hexdigest()

        key = make_key("fn", (42,))
        with Path(cache_file).open("w") as f:
            json.dump({key: 9999}, f)

        call_count = [0]

        @persist_to_file(cache_file)
        def fn(x):
            call_count[0] += 1
            return x + 1

        result = fn(42)
        assert result == _COMPARISON_VALUE_9999
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
        assert r == _COMPARISON_VALUE_20
        assert call_count[0] == 1
        assert Path(cache_dir).is_dir()

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
        assert call_count[0] == _COMPARISON_VALUE_2
        # Each result is in a separate file in cache_dir.
        assert sum(1 for _path in Path(cache_dir).iterdir()) == _COMPARISON_VALUE_2


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
        assert wrapped(3, 4) == _COMPARISON_VALUE_7

    def test_name_and_doc_preserved(self):
        def documented_func(x):
            """My doc."""
            return x

        wrapped = change_repr(documented_func)
        assert wrapped.__name__ == "documented_func"
        assert wrapped.__doc__ == "My doc."
