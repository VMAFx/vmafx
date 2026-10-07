# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The default model this server advertises and uses is the library default.

``VMAF_DEFAULT_MODEL_VERSION`` in ``core/include/libvmaf/model.h`` is the one
definition. Three tool schemas and the request defaults
used to say ``version=vmaf_v0.6.1`` while the library scored with the v1 model.
"""

from __future__ import annotations

import asyncio
import re
from pathlib import Path

from vmaf_mcp import defaultmodel, server

# vmaf_vpl names vmaf_vpl.c's own --model default, not the library default.
_PINNED_TOOLS = {"vmaf_vpl"}

_HEADER = Path(__file__).resolve().parents[3] / "core" / "include" / "libvmaf" / "model.h"


def _header_default() -> str:
    match = re.search(r'^#define VMAF_DEFAULT_MODEL_VERSION "([^"]+)"', _HEADER.read_text(), re.M)
    assert match, f"{_HEADER} does not define VMAF_DEFAULT_MODEL_VERSION"
    return match.group(1)


def test_mirror_equals_the_c_header() -> None:
    assert _header_default() == defaultmodel.DEFAULT_MODEL
    assert f"version={_header_default()}" == defaultmodel.DEFAULT_MODEL_ARG


def test_score_request_defaults_to_the_library_model() -> None:
    request = server.ScoreRequest(
        ref=None, dis=Path("d.yuv"), width=16, height=16, pixfmt="420", bitdepth=8
    )
    assert request.model == defaultmodel.DEFAULT_MODEL_ARG


def test_tool_schemas_advertise_the_library_default() -> None:
    tools = asyncio.run(server._list_tools())
    checked = 0
    for tool in tools:
        # The wire name; the attribute name differs between SDK releases.
        schema = tool.model_dump(by_alias=True)["inputSchema"]
        prop = schema.get("properties", {}).get("model")
        if not prop or "default" not in prop or tool.name in _PINNED_TOOLS:
            continue
        checked += 1
        assert prop["default"] == defaultmodel.DEFAULT_MODEL_ARG, tool.name
    assert checked >= 3, "the guard no longer sees the scoring tools' model defaults"
