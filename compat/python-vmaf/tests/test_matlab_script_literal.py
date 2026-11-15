# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Asset paths reach MATLAB's ``-r`` script only as quoted character vectors."""

from __future__ import annotations

import re
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

from vmaf.core import matlab_feature_extractor
from vmaf.core.matlab_feature_extractor import _matlab_char

# One MATLAB character vector: quote, then any run of non-quotes or doubled quotes, then quote.
CHAR_VECTOR = re.compile(r"'(?:[^']|'')*'")
# What a builder's script must be once each quoted vector is replaced by `S`:
# one call with literal and numeric arguments only, then exit.
ONE_CALL = re.compile(r"^run_[a-z_]+\((?:S|\d+|\[[\d, ]*\])(?:, (?:S|\d+|\[[\d, ]*\]))*\); exit;$")
PAYLOAD = "/clips/x'); system('touch pwned'); %"

EXTRACTORS = [
    matlab_feature_extractor.StrredFeatureExtractor,
    matlab_feature_extractor.StrredOptFeatureExtractor,
    matlab_feature_extractor.SpEEDMatlabFeatureExtractor,
    matlab_feature_extractor.STMADFeatureExtractor,
    matlab_feature_extractor.iCIDFeatureExtractor,
]


def test_plain_path_is_quoted_unchanged() -> None:
    """Positive: a path without quotes is wrapped in one pair of single quotes."""
    assert _matlab_char("/clips/ref_576x324.yuv") == "'/clips/ref_576x324.yuv'"


def test_quote_is_doubled_and_cannot_end_the_vector() -> None:
    """Negative: a quote in the value is doubled, so the payload stays one vector."""
    quoted = _matlab_char(PAYLOAD)
    assert quoted == "'/clips/x''); system(''touch pwned''); %'"
    assert CHAR_VECTOR.fullmatch(quoted)


@pytest.mark.parametrize("value", ["/clips/a\nsystem('x')", "/clips/a\rb", "/clips/a\x00b", "a\tb"])
def test_control_character_is_refused(value: str) -> None:
    """Negative: a newline or other control character would split the one-line script."""
    with pytest.raises(ValueError, match="control character"):
        _matlab_char(value)


@pytest.mark.parametrize(
    "value, expected",
    [("", "''"), ("'", "''''"), ("''", "''''''"), ("/clips/é ü.yuv", "'/clips/é ü.yuv'")],
)
def test_boundary_values(value: str, expected: str) -> None:
    """Boundary: empty value, quotes only, and printable non-ASCII text."""
    assert _matlab_char(value) == expected
    assert CHAR_VECTOR.fullmatch(expected)


@pytest.mark.parametrize("extractor_type", EXTRACTORS)
def test_every_builder_quotes_the_asset_paths(tmp_path: Path, extractor_type: type) -> None:
    """Every MATLAB extractor hands `_run_matlab` one call, whatever the asset path holds."""
    asset = SimpleNamespace(
        ref_procfile_path=PAYLOAD,
        dis_procfile_path=PAYLOAD,
        ref_workfile_path=PAYLOAD,
        dis_workfile_path=PAYLOAD,
        quality_width_height=(576, 324),
        ref_yuv_type="yuv420p",
    )
    extractor = SimpleNamespace(
        MATLAB_WORKSPACE=str(tmp_path),
        logger=None,
        scale_list=[1],
        _get_log_file_path=mock.Mock(return_value=str(tmp_path / "log")),
        _get_workfile_yuv_type=mock.Mock(return_value="yuv420p"),
    )
    with (
        mock.patch.object(
            matlab_feature_extractor.VmafExternalConfig,
            "get_and_assert_matlab",
            return_value="matlab",
        ),
        mock.patch.object(matlab_feature_extractor, "_run_matlab") as run_matlab,
    ):
        extractor_type._generate_result(extractor, asset)
    script = run_matlab.call_args.args[1]
    assert _matlab_char(PAYLOAD) in script
    assert ONE_CALL.fullmatch(CHAR_VECTOR.sub("S", script)), script
