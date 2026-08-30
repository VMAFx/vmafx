# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Additional unit tests for compat/python-vmaf/tools/misc.py — covers
# make_absolute_path, get_cmd_option, cmd_option_exists, index_and_value_of_min,
# check_program_exist, check_scanf_match, get_unique_sorted_list,
# dedup_value_in_dict, empty_object, make_parent_dirs_if_nonexist, and
# delete_dir_if_exists.
"""Extended pytest cases for vmaf.tools.misc."""

from __future__ import annotations

import os

import pytest

from vmaf.tools.misc import (
    check_program_exist,
    check_scanf_match,
    cmd_option_exists,
    dedup_value_in_dict,
    delete_dir_if_exists,
    empty_object,
    get_cmd_option,
    get_unique_sorted_list,
    index_and_value_of_min,
    make_absolute_path,
    make_parent_dirs_if_nonexist,
)

# ---------------------------------------------------------------------------
# make_absolute_path
# ---------------------------------------------------------------------------


class TestMakeAbsolutePath:
    def test_relative_path_prepends_current_dir(self):
        assert make_absolute_path("abc/cde.fg", "/xyz/") == "/xyz/abc/cde.fg"

    def test_absolute_path_returned_unchanged(self):
        assert make_absolute_path("/abc/cde.fg", "/xyz/") == "/abc/cde.fg"

    def test_current_dir_must_end_with_slash(self):
        with pytest.raises(AssertionError):
            make_absolute_path("rel/path.txt", "/no/trailing/slash")


# ---------------------------------------------------------------------------
# get_cmd_option
# ---------------------------------------------------------------------------


class TestGetCmdOption:
    def test_option_at_begin(self):
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 3, 5, "--xyz") == "123"

    def test_option_within_range(self):
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 0, 5, "--xyz") == "123"

    def test_option_at_end_of_range_returns_none(self):
        # Option is at index 3, end=5; option IS in range but its value is at 4 < end.
        # Option at index 4 (--xyz is at 3), so next is 4 which is < end (5) → "123"
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 4, 5, "--xyz") is None

    def test_option_beyond_range_returns_none(self):
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 5, 5, "--xyz") is None

    def test_first_arg_in_range(self):
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 0, 5, "a") == "b"

    def test_second_arg_in_range(self):
        assert get_cmd_option(["a", "b", "c", "--xyz", "123"], 0, 5, "b") == "c"


# ---------------------------------------------------------------------------
# cmd_option_exists
# ---------------------------------------------------------------------------


class TestCmdOptionExists:
    def test_option_exists(self):
        assert cmd_option_exists(["a", "b", "c", "d"], 2, 4, "c") is True

    def test_option_before_begin(self):
        assert cmd_option_exists(["a", "b", "c", "d"], 3, 4, "c") is False

    def test_option_at_last_index(self):
        assert cmd_option_exists(["a", "b", "c", "d"], 3, 4, "d") is True

    def test_option_outside_range(self):
        assert cmd_option_exists(["a", "b", "c", "d"], 2, 4, "a") is False

    def test_option_not_present(self):
        assert cmd_option_exists(["a", "b", "c", "d"], 2, 4, "b") is False


# ---------------------------------------------------------------------------
# index_and_value_of_min
# ---------------------------------------------------------------------------


class TestIndexAndValueOfMin:
    def test_basic(self):
        assert index_and_value_of_min([2, 0, 3]) == (1, 0)

    def test_single_element(self):
        assert index_and_value_of_min([42]) == (0, 42)

    def test_first_min(self):
        assert index_and_value_of_min([1, 2, 3]) == (0, 1)

    def test_last_min(self):
        assert index_and_value_of_min([3, 2, 1]) == (2, 1)


# ---------------------------------------------------------------------------
# check_program_exist
# ---------------------------------------------------------------------------


class TestCheckProgramExist:
    def test_nonexistent_program(self):
        assert check_program_exist("xxxafasd34df") is False

    def test_nonexistent_with_args(self):
        assert check_program_exist("xxxafasd34df f899") is False

    def test_existing_program_ls(self):
        assert check_program_exist("ls") is True

    def test_existing_program_pwd(self):
        assert check_program_exist("pwd") is True


# ---------------------------------------------------------------------------
# check_scanf_match
# ---------------------------------------------------------------------------


class TestCheckScanfMatch:
    def test_matching_frame_pattern(self):
        assert check_scanf_match("frame00000000.icpf", "frame%08d.icpf") is True

    def test_matching_frame_with_nonzero_index(self):
        assert check_scanf_match("frame00000003.icpf", "frame%08d.icpf") is True

    def test_non_matching_prefix(self):
        assert check_scanf_match("gframe00000001.icpff", "frame%08d.icpf") is False

    def test_wildcard_glob_pattern(self):
        assert (
            check_scanf_match(
                "/mnt/hgfs/test/video_1920x1080_30.yuv",
                "/mnt/hgfs/test/video_1920x1080_*.yuv",
            )
            is True
        )

    def test_path_with_double_slash_fails(self):
        assert check_scanf_match("xx/yy//frame00000000.icpf", "xx/yy/frame%08d.icpf") is False


# ---------------------------------------------------------------------------
# get_unique_sorted_list
# ---------------------------------------------------------------------------


class TestGetUniqueSortedList:
    def test_with_duplicates(self):
        assert get_unique_sorted_list([3, 4, 4, 1]) == [1, 3, 4]

    def test_empty_list(self):
        assert get_unique_sorted_list([]) == []

    def test_already_unique(self):
        assert get_unique_sorted_list([1, 2, 3]) == [1, 2, 3]

    def test_all_same(self):
        assert get_unique_sorted_list([5, 5, 5]) == [5]


# ---------------------------------------------------------------------------
# dedup_value_in_dict
# ---------------------------------------------------------------------------


class TestDedupValueInDict:
    def test_dedup_basic(self):
        result = dedup_value_in_dict({"a": 1, "b": 1, "c": 2})
        # Both 'a' and 'b' have value 1; only one should remain (the alphabetically first key).
        assert result == {"a": 1, "c": 2}

    def test_no_duplicates_unchanged(self):
        d = {"x": 10, "y": 20, "z": 30}
        result = dedup_value_in_dict(d)
        assert len(result) == 3

    def test_all_same_value_keeps_one(self):
        d = {"a": 99, "b": 99, "c": 99}
        result = dedup_value_in_dict(d)
        assert len(result) == 1


# ---------------------------------------------------------------------------
# empty_object
# ---------------------------------------------------------------------------


class TestEmptyObject:
    def test_returns_object(self):
        obj = empty_object()
        assert obj is not None

    def test_can_set_attributes(self):
        obj = empty_object()
        obj.foo = "bar"
        assert obj.foo == "bar"


# ---------------------------------------------------------------------------
# make_parent_dirs_if_nonexist
# ---------------------------------------------------------------------------


class TestMakeParentDirsIfNonexist:
    def test_creates_parent_dirs(self, tmp_path):
        target = str(tmp_path / "a" / "b" / "c" / "file.txt")
        make_parent_dirs_if_nonexist(target)
        assert os.path.isdir(str(tmp_path / "a" / "b" / "c"))

    def test_idempotent_if_dirs_exist(self, tmp_path):
        target = str(tmp_path / "existing" / "file.txt")
        make_parent_dirs_if_nonexist(target)
        # Should not raise even if dirs already exist.
        make_parent_dirs_if_nonexist(target)


# ---------------------------------------------------------------------------
# delete_dir_if_exists
# ---------------------------------------------------------------------------


class TestDeleteDirIfExists:
    def test_deletes_existing_empty_dir(self, tmp_path):
        d = tmp_path / "to_delete"
        d.mkdir()
        assert d.is_dir()
        delete_dir_if_exists(str(d))
        assert not d.exists()

    def test_noop_if_dir_does_not_exist(self, tmp_path):
        nonexistent = str(tmp_path / "no_such_dir")
        # Should not raise.
        delete_dir_if_exists(nonexistent)
