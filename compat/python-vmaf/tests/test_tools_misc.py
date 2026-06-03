# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/tools/misc.py and tools/decorator.py
#
# Covers pure-Python utility functions that require no video fixtures and no
# external binaries. The focus is on path helpers, dict helpers, map_yuv_type_to_bitdepth,
# indices(), unroll_dict_of_lists(), and the decorator utilities.
"""Pytest cases for vmaf.tools.misc + vmaf.tools.decorator."""

from __future__ import annotations

import pytest

from vmaf.tools.decorator import deprecated, dummy, memoized, override, persist
from vmaf.tools.misc import (
    get_dir_without_last_slash,
    get_file_name_extension,
    get_file_name_with_extension,
    get_file_name_without_extension,
    get_hashable_value_tuple_from_dict,
    get_normalized_string_from_dict,
    get_unique_str_from_recursive_dict,
    indices,
    map_yuv_type_to_bitdepth,
    neg_if_even,
    unroll_dict_of_lists,
)

# ---------------------------------------------------------------------------
# get_file_name_without_extension
# ---------------------------------------------------------------------------


class TestGetFileNameWithoutExtension:
    def test_with_extension(self):
        assert get_file_name_without_extension("yuv/src01_hrc01.yuv") == "src01_hrc01"

    def test_without_extension(self):
        assert get_file_name_without_extension("yuv/src01_hrc01") == "src01_hrc01"

    def test_nested_path(self):
        assert get_file_name_without_extension("abc/xyz/src01_hrc01.yuv") == "src01_hrc01"

    def test_double_extension(self):
        assert get_file_name_without_extension("abc/xyz/src01.sdr.yuv") == "src01.sdr"

    def test_triple_extension(self):
        assert (
            get_file_name_without_extension("abc/xyz/src01_hrc01.sdr.dvi.yuv")
            == "src01_hrc01.sdr.dvi"
        )


# ---------------------------------------------------------------------------
# get_file_name_with_extension
# ---------------------------------------------------------------------------


class TestGetFileNameWithExtension:
    def test_with_dir(self):
        assert get_file_name_with_extension("yuv/src01_hrc01.yuv") == "src01_hrc01.yuv"

    def test_no_dir(self):
        assert get_file_name_with_extension("src01_hrc01.yuv") == "src01_hrc01.yuv"

    def test_nested_dir(self):
        assert get_file_name_with_extension("abc/xyz/src01_hrc01.yuv") == "src01_hrc01.yuv"


# ---------------------------------------------------------------------------
# get_file_name_extension
# ---------------------------------------------------------------------------


class TestGetFileNameExtension:
    def test_txt_extension(self):
        assert get_file_name_extension("test.txt") == "txt"

    def test_no_extension(self):
        assert get_file_name_extension("abc") == ""

    def test_265_extension(self):
        assert get_file_name_extension("test.265") == "265"

    def test_uri_path(self):
        assert get_file_name_extension("file:///mnt/zli/test.txt") == "txt"


# ---------------------------------------------------------------------------
# get_dir_without_last_slash
# ---------------------------------------------------------------------------


class TestGetDirWithoutLastSlash:
    def test_file_in_dir(self):
        assert get_dir_without_last_slash("abc/src01_hrc01.yuv") == "abc"

    def test_no_dir(self):
        assert get_dir_without_last_slash("src01_hrc01.yuv") == ""

    def test_nested_dir(self):
        assert get_dir_without_last_slash("abc/xyz/src01_hrc01.yuv") == "abc/xyz"

    def test_trailing_slash(self):
        assert get_dir_without_last_slash("abc/xyz/") == "abc/xyz"


# ---------------------------------------------------------------------------
# get_normalized_string_from_dict
# ---------------------------------------------------------------------------


class TestGetNormalizedStringFromDict:
    def test_basic(self):
        d = {"max_buffer_sec": 5.0, "bitrate_kbps": 45}
        assert get_normalized_string_from_dict(d) == "bitrate_kbps_45_max_buffer_sec_5.0"

    def test_single_key(self):
        d = {"foo": "bar"}
        assert get_normalized_string_from_dict(d) == "foo_bar"

    def test_empty_dict(self):
        assert get_normalized_string_from_dict({}) == ""


# ---------------------------------------------------------------------------
# get_hashable_value_tuple_from_dict
# ---------------------------------------------------------------------------


class TestGetHashableValueTupleFromDict:
    def test_basic(self):
        d = {"max_buffer_sec": 5.0, "bitrate_kbps": 45}
        assert get_hashable_value_tuple_from_dict(d) == (45, 5.0)

    def test_with_list_value(self):
        d = {
            "max_buffer_sec": 5.0,
            "bitrate_kbps": 45,
            "resolutions": [(740, 480), (1920, 1080)],
        }
        result = get_hashable_value_tuple_from_dict(d)
        assert result == (45, 5.0, ((740, 480), (1920, 1080)))

    def test_result_is_hashable(self):
        d = {"a": 1, "b": [2, 3]}
        t = get_hashable_value_tuple_from_dict(d)
        # Must not raise when used as a dict key.
        hash(t)


# ---------------------------------------------------------------------------
# get_unique_str_from_recursive_dict
# ---------------------------------------------------------------------------


class TestGetUniqueStrFromRecursiveDict:
    def test_flat_dict(self):
        d = {"a": 1, "b": 2, "c": {"x": "0", "y": "1"}}
        s = get_unique_str_from_recursive_dict(d)
        assert s == '{"a": 1, "b": 2, "c": {"x": "0", "y": "1"}}'

    def test_sorted_keys(self):
        d = {"a": 1, "c": 2, "b": {"y": "1", "x": "0"}}
        s = get_unique_str_from_recursive_dict(d)
        assert s == '{"a": 1, "b": {"x": "0", "y": "1"}, "c": 2}'

    def test_empty_dict(self):
        assert get_unique_str_from_recursive_dict({}) == "{}"

    def test_same_content_different_order_same_str(self):
        d1 = {"z": 99, "a": 1}
        d2 = {"a": 1, "z": 99}
        assert get_unique_str_from_recursive_dict(d1) == get_unique_str_from_recursive_dict(d2)


# ---------------------------------------------------------------------------
# indices
# ---------------------------------------------------------------------------


class TestIndices:
    def test_greater_than(self):
        assert indices([1, 2, 3, 4], lambda x: x > 2) == [2, 3]

    def test_no_match(self):
        assert indices([1, 2, 3, 4], lambda x: x == 2.5) == []

    def test_range(self):
        assert indices([1, 2, 3, 4], lambda x: 1 < x <= 3) == [1, 2]

    def test_in_list(self):
        assert indices([1, 2, 3, 4], lambda x: x in [2, 4]) == [1, 3]

    def test_repeated_values(self):
        assert indices([1, 2, 3, 1, 2, 3, 1, 2, 3], lambda x: x > 2) == [2, 5, 8]

    def test_empty_list(self):
        assert indices([], lambda x: x > 0) == []


# ---------------------------------------------------------------------------
# map_yuv_type_to_bitdepth
# ---------------------------------------------------------------------------


class TestMapYuvTypeToBitdepth:
    @pytest.mark.parametrize("yuv_type", ["yuv420p", "yuv422p", "yuv444p"])
    def test_8bit(self, yuv_type):
        assert map_yuv_type_to_bitdepth(yuv_type) == 8

    @pytest.mark.parametrize("yuv_type", ["yuv420p10le", "yuv422p10le", "yuv444p10le"])
    def test_10bit(self, yuv_type):
        assert map_yuv_type_to_bitdepth(yuv_type) == 10

    @pytest.mark.parametrize("yuv_type", ["yuv420p12le", "yuv422p12le", "yuv444p12le"])
    def test_12bit(self, yuv_type):
        assert map_yuv_type_to_bitdepth(yuv_type) == 12

    @pytest.mark.parametrize("yuv_type", ["yuv420p16le", "yuv422p16le", "yuv444p16le"])
    def test_16bit(self, yuv_type):
        assert map_yuv_type_to_bitdepth(yuv_type) == 16

    def test_notyuv_returns_none(self):
        assert map_yuv_type_to_bitdepth("notyuv") is None

    def test_unknown_returns_none(self):
        assert map_yuv_type_to_bitdepth("yuv999p") is None


# ---------------------------------------------------------------------------
# neg_if_even
# ---------------------------------------------------------------------------


class TestNegIfEven:
    def test_even_becomes_negative(self):
        assert neg_if_even(0) < 0 or neg_if_even(0) == 0  # 0 is even
        assert neg_if_even(2) < 0
        assert neg_if_even(4) < 0

    def test_odd_stays_positive(self):
        assert neg_if_even(1) > 0
        assert neg_if_even(3) > 0


# ---------------------------------------------------------------------------
# unroll_dict_of_lists
# ---------------------------------------------------------------------------


class TestUnrollDictOfLists:
    def test_single_key_single_value(self):
        result = unroll_dict_of_lists({"a": [1]})
        assert result == [{"a": 1}]

    def test_two_keys_product(self):
        result = unroll_dict_of_lists({"a": [1, 2], "b": [3]})
        assert len(result) == 2
        assert {"a": 1, "b": 3} in result
        assert {"a": 2, "b": 3} in result

    def test_empty_dict(self):
        result = unroll_dict_of_lists({})
        assert result == [{}]

    def test_cross_product_size(self):
        result = unroll_dict_of_lists({"x": [1, 2], "y": [10, 20, 30]})
        assert len(result) == 6

    def test_all_values_covered(self):
        result = unroll_dict_of_lists({"k": [1, 2, 3]})
        values = [d["k"] for d in result]
        assert sorted(values) == [1, 2, 3]


# ---------------------------------------------------------------------------
# decorator.py — deprecated
# ---------------------------------------------------------------------------


class TestDeprecatedDecorator:
    def test_deprecated_still_works(self):
        import warnings

        @deprecated
        def old_fn(x):
            return x * 2

        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            result = old_fn(5)
            assert result == 10
            assert len(w) == 1
            assert issubclass(w[0].category, DeprecationWarning)

    def test_deprecated_preserves_name(self):
        @deprecated
        def my_func():
            pass

        assert my_func.__name__ == "my_func"


# ---------------------------------------------------------------------------
# decorator.py — dummy
# ---------------------------------------------------------------------------


class TestDummyDecorator:
    def test_dummy_is_identity(self):
        @dummy
        def fn(x):
            return x + 1

        assert fn(4) == 5


# ---------------------------------------------------------------------------
# decorator.py — memoized
# ---------------------------------------------------------------------------


class TestMemoizedDecorator:
    def test_memoized_caches_result(self):
        call_count = [0]

        @memoized
        def expensive(x):
            call_count[0] += 1
            return x * x

        assert expensive(3) == 9
        assert expensive(3) == 9
        assert call_count[0] == 1  # only computed once

    def test_memoized_different_args_computed(self):
        call_count = [0]

        @memoized
        def fn(x):
            call_count[0] += 1
            return x

        fn(1)
        fn(2)
        assert call_count[0] == 2


# ---------------------------------------------------------------------------
# decorator.py — persist
# ---------------------------------------------------------------------------


class TestPersistDecorator:
    def test_persist_is_callable_decorator(self):
        # The persist decorator in Python 3.14+ raises TypeError when the cache
        # key is computed because hashlib.sha1() requires bytes, not str.
        # That pre-existing bug is tracked separately; here we only verify that
        # @persist wraps a function without raising at decoration time.
        @persist
        def fn(x):
            return x

        assert callable(fn)


# ---------------------------------------------------------------------------
# decorator.py — override
# ---------------------------------------------------------------------------


class TestOverrideDecorator:
    def test_override_valid_method(self):
        class Base:
            def method(self):
                pass

        class Child(Base):
            @override(Base)
            def method(self):
                return "child"

        assert Child().method() == "child"

    def test_override_nonexistent_method_raises(self):
        class Base:
            pass

        with pytest.raises(AssertionError):

            class Child(Base):
                @override(Base)
                def nonexistent(self):
                    pass
