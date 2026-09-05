# Copyright 2026 Lusoris. All rights reserved.
# Use of this source code is governed by the BSD-3-Clause-Plus-Patent
# license that can be found in the LICENSE file.

"""Tests for the sidecar-binary MCP tools (epic #1240 item b).

The four tools bridge core/tools/vmaf_per_shot.c, vmaf_roi.c, vmaf_bench.c and
vmaf_vpl.c.  These tests pin the argument bounds against the C parsers, the
argv construction, the response shapes, and the binary-resolution order.  The
Go/Python byte-parity of the argv builders is enforced separately by
cmd/vmafx-mcp/sidecar_parity_test.go.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

import pytest

from vmaf_mcp import server as srv

REPO = Path(__file__).resolve().parents[3]
FIXTURE = REPO / "model" / "vmaf_v0.6.1.json"

SIDECAR_TOOLS = ("vmaf_per_shot", "vmaf_roi", "vmaf_bench", "vmaf_vpl")


# ---------------------------------------------------------------------------
# Tool registration
# ---------------------------------------------------------------------------


@pytest.mark.asyncio
async def test_sidecar_tools_are_registered() -> None:
    names = {t.name for t in await srv._list_tools()}
    for tool in SIDECAR_TOOLS:
        assert tool in names, f"{tool} missing from _list_tools()"


@pytest.mark.asyncio
async def test_sidecar_tool_required_fields() -> None:
    schemas = {t.name: t.input_schema for t in await srv._list_tools()}
    assert schemas["vmaf_per_shot"]["required"] == ["reference", "width", "height"]
    assert schemas["vmaf_roi"]["required"] == ["reference", "width", "height", "frame"]
    assert "required" not in schemas["vmaf_bench"]
    assert schemas["vmaf_vpl"]["required"] == ["ref", "dis"]


# ---------------------------------------------------------------------------
# Float formatting — the Go/Python argv-parity linchpin
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("value", "expected"),
    [
        (90.0, "90"),
        (90, "90"),
        (93.5, "93.5"),
        (12.0, "12"),
        (0.0, "0"),
        (6.25, "6.25"),
        (1e-05, "0.00001"),
    ],
)
def test_fmt_float_matches_go_shortest_form(value: float, expected: str) -> None:
    """Go writes strconv.FormatFloat(v, 'f', -1, 64): shortest, never exponent."""
    assert srv._fmt_float(value) == expected


# ---------------------------------------------------------------------------
# Bounds validation — every range comes from the C parsers
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("args", "match"),
    [
        ({"reference": str(FIXTURE), "width": 8, "height": 324}, "between 16 and 65535"),
        ({"reference": str(FIXTURE), "width": 576, "height": 70000}, "between 16 and 65535"),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "pixel_format": "410"},
            "invalid pixel_format",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "bitdepth": 14},
            "invalid bitdepth",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "target_vmaf": 101},
            "between 0 and 100",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "crf_max": 64},
            "between 0 and 63",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "crf_min": 40, "crf_max": 20},
            "must not exceed",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "diff_threshold": 300},
            "between 0 and 255",
        ),
        (
            {"reference": str(FIXTURE), "width": 576, "height": 324, "format": "yaml"},
            "must be json or csv",
        ),
        ({"reference": "/etc/passwd", "width": 576, "height": 324}, "not under an allowlisted"),
    ],
)
def test_per_shot_rejects_out_of_range(args: dict, match: str) -> None:
    with pytest.raises((ValueError, FileNotFoundError), match=match):
        srv._build_per_shot_argv("BIN", args)


@pytest.mark.parametrize(
    ("args", "match"),
    [
        (
            {"reference": str(FIXTURE), "width": 0, "height": 1080, "frame": 0},
            "between 1 and 16384",
        ),
        (
            {"reference": str(FIXTURE), "width": 1920, "height": 1080, "frame": 1000001},
            "between 0 and 1000000",
        ),
        (
            {"reference": str(FIXTURE), "width": 1920, "height": 1080, "frame": 0, "ctu_size": 4},
            "between 8 and 128",
        ),
        (
            {
                "reference": str(FIXTURE),
                "width": 1920,
                "height": 1080,
                "frame": 0,
                "encoder": "vp9",
            },
            "must be x265 or svt-av1",
        ),
        (
            {"reference": str(FIXTURE), "width": 1920, "height": 1080, "frame": 0, "strength": 65},
            "between 0 and 64",
        ),
    ],
)
def test_roi_rejects_out_of_range(args: dict, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        srv._build_roi_argv("BIN", "/tmp/out.bin", args)


@pytest.mark.parametrize(
    ("args", "match"),
    [
        ({"frames": 1}, "between 2 and 48"),
        ({"frames": 49}, "between 2 and 48"),
        ({"resolution": "800x600"}, "invalid resolution"),
        ({"bpc": 9}, "invalid bpc"),
        ({"data_dir": "/etc"}, "not under an allowlisted"),
    ],
)
def test_bench_rejects_out_of_range(args: dict, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        srv._build_bench_argv("BIN", args)


@pytest.mark.parametrize(
    ("args", "match"),
    [
        (
            {"ref": str(FIXTURE), "dis": str(FIXTURE), "model": "/models/x.json"},
            "not a path",
        ),
        ({"ref": str(FIXTURE), "dis": str(FIXTURE), "frames": -1}, "frames must be >= 0"),
        ({"ref": str(FIXTURE), "dis": str(FIXTURE), "device": -1}, "device must be >= 0"),
        (
            {"ref": str(FIXTURE), "dis": str(FIXTURE), "render_node": "/etc/passwd"},
            "invalid render_node",
        ),
        (
            {
                "ref": str(FIXTURE),
                "dis": str(FIXTURE),
                "render_node": "/dev/dri/../../etc/passwd",
            },
            "invalid render_node",
        ),
    ],
)
def test_vpl_rejects_out_of_range(args: dict, match: str) -> None:
    with pytest.raises(ValueError, match=match):
        srv._build_vpl_argv("BIN", args)


# ---------------------------------------------------------------------------
# argv construction
# ---------------------------------------------------------------------------


def test_per_shot_argv_defaults() -> None:
    argv, fmt = srv._build_per_shot_argv(
        "BIN", {"reference": str(FIXTURE), "width": 576, "height": 324}
    )
    assert fmt == "json"
    assert argv == [
        "BIN",
        "--reference",
        str(FIXTURE.resolve()),
        "--width",
        "576",
        "--height",
        "324",
        "--pixel_format",
        "420",
        "--bitdepth",
        "8",
        "--output",
        "-",
        "--target-vmaf",
        "90",
        "--crf-min",
        "18",
        "--crf-max",
        "35",
        "--format",
        "json",
    ]
    # --diff-threshold is only emitted when the caller supplies it, so the C
    # default (12.0) stays authoritative.
    assert "--diff-threshold" not in argv


def test_bench_argv_is_empty_without_arguments() -> None:
    argv, validate = srv._build_bench_argv("BIN", {})
    assert argv == ["BIN"]
    assert validate is False


def test_vpl_argv_always_pins_device_and_render_node() -> None:
    argv, params = srv._build_vpl_argv("BIN", {"ref": str(FIXTURE), "dis": str(FIXTURE)})
    assert params == {"model": "vmaf_v0.6.1", "device": 0, "render_node": "/dev/dri/renderD128"}
    assert "--render-node" in argv
    assert "--fallback" not in argv


# ---------------------------------------------------------------------------
# Binary resolution + fast-fail
# ---------------------------------------------------------------------------


def test_sidecar_binary_env_override(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("VMAF_ROI_BIN", "/opt/custom/vmaf_roi")
    assert srv._sidecar_binary("vmaf_roi") == Path("/opt/custom/vmaf_roi")


def test_sidecar_binary_rejects_unknown_name() -> None:
    with pytest.raises(ValueError, match="unknown sidecar binary"):
        srv._sidecar_binary("not_a_sidecar")


@pytest.mark.parametrize(
    ("tool", "args"),
    [
        ("vmaf_per_shot", {"reference": str(FIXTURE), "width": 576, "height": 324}),
        ("vmaf_roi", {"reference": str(FIXTURE), "width": 576, "height": 324, "frame": 0}),
        ("vmaf_bench", {}),
        ("vmaf_vpl", {"ref": str(FIXTURE), "dis": str(FIXTURE)}),
    ],
)
def test_sidecar_tools_fail_fast_without_binary(
    tool: str, args: dict, monkeypatch: pytest.MonkeyPatch
) -> None:
    for name, env in srv._SIDECAR_BINARY_ENV.items():
        monkeypatch.setenv(env, f"/nonexistent/{name}")
    with pytest.raises(FileNotFoundError, match="binary not found"):
        asyncio.run(srv._call_tool_dispatch(tool, args, None))


# ---------------------------------------------------------------------------
# Response shaping
# ---------------------------------------------------------------------------


def test_per_shot_parses_the_json_plan(monkeypatch: pytest.MonkeyPatch) -> None:
    plan = {"target_vmaf": 90.0, "crf_min": 18, "crf_max": 35, "shots": []}

    async def fake_run(argv: list[str]) -> tuple[str, str, int]:
        assert argv[0] == "BIN"
        return json.dumps(plan), "wrote 0 shot(s)", 0

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    out = asyncio.run(srv._vmaf_per_shot({"reference": str(FIXTURE), "width": 576, "height": 324}))
    assert out["format"] == "json"
    assert out["plan"] == plan
    assert out["exit_code"] == 0


def test_per_shot_reports_a_non_zero_exit_as_an_error(monkeypatch: pytest.MonkeyPatch) -> None:
    async def fake_run(_argv: list[str]) -> tuple[str, str, int]:
        return "", "vmaf-perShot: cannot open output", 1

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    with pytest.raises(RuntimeError, match="vmaf-perShot exited 1"):
        asyncio.run(srv._vmaf_per_shot({"reference": str(FIXTURE), "width": 576, "height": 324}))


def test_bench_validate_mode_reports_failure_without_raising(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """--validate exits 1 to say "the comparison found deltas" — a result, not an error."""

    async def fake_run(_argv: list[str]) -> tuple[str, str, int]:
        return "FAILURES\n", "", 1

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    out = asyncio.run(srv._vmaf_bench({"validate": True}))
    assert out["mode"] == "validate"
    assert out["validation_failed"] is True
    assert out["exit_code"] == 1


def test_bench_benchmark_mode_raises_on_a_non_zero_exit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    async def fake_run(_argv: list[str]) -> tuple[str, str, int]:
        return "", "boom", 1

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    with pytest.raises(RuntimeError, match="vmaf_bench exited 1"):
        asyncio.run(srv._vmaf_bench({}))


def test_vpl_parses_the_score_summary(monkeypatch: pytest.MonkeyPatch) -> None:
    stdout = "Frames: 12\nTime:   0.400 s (30.0 FPS)\nVMAF:   96.123456 (mean)\n"

    async def fake_run(_argv: list[str]) -> tuple[str, str, int]:
        return stdout, "", 0

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    out = asyncio.run(srv._vmaf_vpl({"ref": str(FIXTURE), "dis": str(FIXTURE)}))
    assert out["vmaf_score"] == pytest.approx(96.123456)
    assert out["frames_processed"] == 12
    assert out["render_node"] == "/dev/dri/renderD128"


def test_roi_x265_returns_qpfile_text(monkeypatch: pytest.MonkeyPatch) -> None:
    qpfile = "# vmaf-roi qpfile (x265, --qpfile-style)\n0 0\n0 0\n"

    async def fake_run(argv: list[str]) -> tuple[str, str, int]:
        out_path = argv[argv.index("--output") + 1]
        Path(out_path).write_text(qpfile, encoding="utf-8")
        return "", "", 0

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    out = asyncio.run(
        srv._vmaf_roi({"reference": str(FIXTURE), "width": 128, "height": 128, "frame": 0})
    )
    assert out["encoder"] == "x265"
    assert out["sidecar_fmt"] == "qpfile"
    assert out["qpfile"] == qpfile
    assert out["grid_cols"] == 2
    assert out["grid_rows"] == 2
    assert out["saliency"] == "placeholder"


def test_roi_svtav1_returns_base64_map(monkeypatch: pytest.MonkeyPatch) -> None:
    raw = bytes([0, 1, 2, 3])

    async def fake_run(argv: list[str]) -> tuple[str, str, int]:
        Path(argv[argv.index("--output") + 1]).write_bytes(raw)
        return "", "", 0

    monkeypatch.setattr(srv, "_resolve_sidecar", lambda _name: Path("BIN"))
    monkeypatch.setattr(srv, "_run_sidecar", fake_run)
    out = asyncio.run(
        srv._vmaf_roi(
            {
                "reference": str(FIXTURE),
                "width": 128,
                "height": 128,
                "frame": 0,
                "encoder": "svt-av1",
            }
        )
    )
    assert out["sidecar_fmt"] == "roi_map_int8"
    assert out["roi_map_base64"] == "AAECAw=="
    assert out["bytes"] == 4
    assert "qpfile" not in out


def test_validate_dir_accepts_an_allowlisted_directory() -> None:
    assert srv._validate_dir(str(REPO / "testdata")).name == "testdata"


def test_validate_dir_rejects_a_file() -> None:
    with pytest.raises(NotADirectoryError):
        srv._validate_dir(str(FIXTURE))
