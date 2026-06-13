# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the 2026-05-18 BBB end-to-end v4 bug cluster.

The v4 probe (after PR #1256 / ADR-0499 closed the v3 cluster) flagged
three findings — pinned here one regression each per ADR-0501:

* **V4-A**: *Retired by ADR-0726 (2026-05-28).* The original probe
  used ``vmaf --backend vulkan`` to drive the strict-mode refusal
  path. With the Vulkan backend removed, ``vmaf`` now rejects that
  value at CLI parse time and the strict-mode exit-byte contract is
  exercised by the equivalent CUDA / SYCL / HIP pins elsewhere.
* **V4-B**: ``vmaf-tune ladder`` against a multi-resolution grid must
  (a) downscale the *reference* leg to the rung target before scoring
  — otherwise the libvmaf CLI mis-parses raw planar bytes at the wrong
  geometry and emits a catastrophic VMAF (~21 instead of ~93); and
  (b) emit a top-level ``samples`` array in the JSON descriptor so
  the ``report`` consumer can render the pre-hull cloud.
* **V4-C**: ``vmaf-tune report`` must distinguish ``encoder unavailable``
  rows (infrastructure gap — encoder not built into ffmpeg) from real
  encode failures. Unavailable rows raise ``degraded=true`` but do
  *not* gate ``ok``; only real failures flip ``ok`` to ``false``.

The order mirrors the v4 bug log.
"""

from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import main as cli_main  # noqa: E402
from vmaftune.corpus import (  # noqa: E402
    CorpusJob,
    CorpusOptions,
    _maybe_decode_reference,
    iter_rows,
)
from vmaftune.ladder import LadderPoint, Rendition, _emit_json, emit_manifest  # noqa: E402


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


# ---------------------------------------------------------------------------
# Bug #V4-A — RETIRED 2026-05-28 by ADR-0726
# ---------------------------------------------------------------------------
#
# The original V4-A reproducer drove the strict-mode --backend refusal
# path with ``vmaf --backend vulkan`` against a CPU-only host. ADR-0726
# removed the Vulkan backend entirely, so the CLI now rejects
# ``vulkan`` at argparse time and the refusal path is unreachable
# through that value. The strict-mode exit-byte contract is still
# exercised against the surviving backends (CUDA / SYCL / HIP) in
# the corresponding pins under ``tools/vmaf-tune/tests`` and
# ``mcp-server/vmaf-mcp/tests``.


def _find_yuv_resource_root() -> Path | None:
    """Walk up from the test file to find ``python/test/resource/yuv``.

    Allows the test to run from either the repo root or a worktree
    checkout. Returns ``None`` when the fixture tree is missing —
    the caller skips.
    """
    here = _HERE.resolve()
    for candidate in [here, *here.parents]:
        target = candidate / "python" / "test" / "resource" / "yuv"
        if target.is_dir():
            return target
    return None


# ---------------------------------------------------------------------------
# Bug #V4-B — ladder grid: cross-res reference is downscaled before scoring
# ---------------------------------------------------------------------------


def test_maybe_decode_reference_scales_to_rung_target(tmp_path: Path, monkeypatch: Any) -> None:
    """When target dims are passed, the reference decode runs ``-vf scale=W:H``."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
        Path(argv[-1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[-1]).write_bytes(b"\x00" * 64)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    encode_dir = tmp_path / "enc"

    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=encode_dir,
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
        target_width=1280,
        target_height=720,
    )
    assert rc == 0
    assert decoded.exists()
    # Per-rung sidecar filename embeds the target dims so cross-res
    # rungs in the same sweep don't collide on a stale path.
    assert "1280x720" in decoded.name
    assert captured, "ffmpeg decode never invoked"
    argv = captured[0]
    assert "-vf" in argv, argv
    assert argv[argv.index("-vf") + 1] == "scale=1280:720"


def test_maybe_decode_reference_no_scale_when_target_dims_omitted(
    tmp_path: Path,
) -> None:
    """Legacy path — no target dims means no scale filter (back-compat)."""
    captured: list[list[str]] = []

    def _runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
        Path(argv[-1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[-1]).write_bytes(b"\x00" * 64)
        return _FakeCompleted(returncode=0)

    src = tmp_path / "ref.mp4"
    src.write_bytes(b"\x00")
    decoded, rc = _maybe_decode_reference(
        src,
        encode_dir=tmp_path / "enc",
        pix_fmt="yuv420p",
        duration_s=2.0,
        ffmpeg_bin="ffmpeg",
        runner=_runner,
    )
    assert rc == 0
    assert decoded.exists()
    assert captured
    assert "-vf" not in captured[0], captured[0]


def test_iter_rows_passes_rung_target_to_reference_decode(tmp_path: Path, monkeypatch: Any) -> None:
    """``iter_rows`` propagates the rung target to the reference decode.

    Pins the ADR-0501 wiring: when ``CorpusJob.src_width/src_height``
    differ from ``width/height`` the reference decode receives the
    rung target as the ``target_width/target_height`` kwargs so the
    libvmaf CLI reads ref + dist at the same geometry.
    """
    import vmaftune.corpus as corpus_mod

    job = CorpusJob(
        source=tmp_path / "src.yuv",
        width=1280,
        height=720,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        cells=(("medium", 28),),
        src_width=1920,
        src_height=1080,
    )
    (tmp_path / "src.yuv").write_bytes(b"\x00")
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "enc",
        src_sha256=False,
    )

    sp_argvs: list[list[str]] = []

    def _fake_sp_run(argv: list[Any], **_kw: Any) -> _FakeCompleted:
        sp_argvs.append([str(a) for a in argv])
        Path(argv[-1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", _fake_sp_run)
    monkeypatch.setattr(corpus_mod, "_sha256_file", lambda *_a, **_kw: "")

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\n")

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[argv.index("--output") + 1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('{"pooled_metrics": {"vmaf": {"mean": 92.0}}}\n')
        return _FakeCompleted(returncode=0, stderr="VMAF version 3.0.0-test\n")

    rows = list(iter_rows(job, opts, encode_runner=_enc_runner, score_runner=_score_runner))
    assert rows
    ref_decodes = [a for a in sp_argvs if any("ref.decoded" in str(x) for x in a)]
    assert ref_decodes, f"reference decode never ran (sp_argvs={sp_argvs!r})"
    ref_argv = ref_decodes[0]
    assert "-vf" in ref_argv, ref_argv
    assert ref_argv[ref_argv.index("-vf") + 1] == "scale=1280:720"


def test_ladder_json_emits_samples_array() -> None:
    """``vmaf-tune-ladder/v1`` JSON must include a ``samples`` array.

    Bug #V4-B-c: the previous emitter dropped the pre-hull sample
    cloud, so ``vmaf-tune report --ladder-json`` saw ``ladder_samples=0``
    and could not render the hull plot. ADR-0501 plumbs the raw
    ``LadderPoint`` cloud through ``build_and_emit -> emit_manifest``.
    """
    samples = [
        LadderPoint(width=1920, height=1080, bitrate_kbps=8000.0, vmaf=95.0, crf=23),
        LadderPoint(width=1920, height=1080, bitrate_kbps=4000.0, vmaf=92.0, crf=28),
        LadderPoint(width=1280, height=720, bitrate_kbps=2000.0, vmaf=88.0, crf=23),
        LadderPoint(width=1280, height=720, bitrate_kbps=1000.0, vmaf=84.0, crf=28),
    ]
    rungs = [
        Rendition(width=1280, height=720, bitrate_kbps=1000.0, vmaf=84.0, crf=28),
        Rendition(width=1920, height=1080, bitrate_kbps=8000.0, vmaf=95.0, crf=23),
    ]
    out = emit_manifest(rungs, format="json", samples=samples)
    payload = json.loads(out)
    assert payload["schema"] == "vmaf-tune-ladder/v1"
    assert "renditions" in payload
    assert "samples" in payload
    assert len(payload["samples"]) == 4
    s0 = payload["samples"][0]
    assert {
        "width",
        "height",
        "bitrate_kbps",
        "bandwidth_bps",
        "vmaf",
        "crf",
    } <= s0.keys()
    # Samples are sorted by (pixel_count, bitrate_kbps) ascending so a
    # downstream plotter can iterate without re-sorting.
    pixel_counts = [s["width"] * s["height"] for s in payload["samples"]]
    assert pixel_counts == sorted(pixel_counts)


def test_ladder_json_emits_empty_samples_when_unset() -> None:
    """JSON emitter still includes ``samples`` (empty) when no cloud is wired.

    Guards downstream consumers that always read ``.samples`` from
    flipping to a KeyError on the legacy code path.
    """
    rungs = [
        Rendition(width=1920, height=1080, bitrate_kbps=8000.0, vmaf=95.0, crf=23),
    ]
    out = _emit_json(rungs)
    payload = json.loads(out)
    assert payload.get("samples") == []


def test_build_and_emit_threads_samples_into_json(tmp_path: Path) -> None:
    """``build_and_emit(format='json')`` must thread the sampler cloud through.

    Uses a synthetic ``sampler=`` stub to avoid real encodes; asserts
    every ``(resolution, target_vmaf)`` cell shows up in the JSON
    ``samples`` array.
    """
    from vmaftune.ladder import build_and_emit

    cells_seen: list[tuple[int, int, float]] = []

    def _stub_sampler(
        src: Path, encoder: str, width: int, height: int, target_vmaf: float
    ) -> LadderPoint:
        cells_seen.append((width, height, target_vmaf))
        # Synthesise a plausible (bitrate, vmaf) point keyed by the cell
        # so each cell produces a distinct sample.
        kbps = 1000.0 * (width * height / (1280 * 720)) * (target_vmaf / 90.0)
        return LadderPoint(
            width=width,
            height=height,
            bitrate_kbps=kbps,
            vmaf=target_vmaf,
            crf=28,
        )

    out = build_and_emit(
        src=tmp_path / "src.yuv",
        encoder="libx264",
        resolutions=((1920, 1080), (1280, 720)),
        target_vmafs=(88.0, 92.0),
        quality_tiers=4,
        format="json",
        sampler=_stub_sampler,
    )
    assert len(cells_seen) == 4
    payload = json.loads(out)
    # ADR-0502 / V5-3: samples are now de-duplicated by
    # ``(width, height, crf)`` so two target-VMAFs that converge on
    # the same CRF emit a single sample row (not two). The V4 stub
    # returns ``crf=28`` for every cell, so two resolutions x two
    # targets collapses to two unique rows (one per resolution).
    assert len(payload["samples"]) == 2
    sample_keys = {(s["width"], s["height"], s["crf"]) for s in payload["samples"]}
    expected_keys = {(w, h, 28) for (w, h, _) in cells_seen}
    assert sample_keys == expected_keys


# ---------------------------------------------------------------------------
# Bug #V4-C — report distinguishes encoder-unavailable from real failure
# ---------------------------------------------------------------------------


def test_report_encoder_unavailable_does_not_gate_ok(tmp_path: Path, capsys: Any) -> None:
    """An ``encoder unavailable`` row should keep ``ok=true`` and set ``degraded=true``.

    Reproducer for Bug #V4-C: the v4 probe ran ``compare`` with three
    encoders, one of which (libsvtav1) is not present in the dev-mcp
    ffmpeg build. The bisect discriminator marks the row
    ``ok=false, error="encoder unavailable (libsvtav1): …"``. The
    report aggregation used to flip the run to ``ok=false`` purely
    because of this infrastructure-gap row; ADR-0501 surfaces a new
    ``degraded`` flag for it instead.
    """
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * (1920 * 1080 * 3 // 2))
    compare_json = tmp_path / "compare.json"
    compare_json.write_text(
        json.dumps(
            {
                "rows": [
                    {
                        "codec": "libx264",
                        "encoder_version": "n6.0",
                        "best_crf": 26,
                        "bitrate_kbps": 4000.0,
                        "encode_time_ms": 100.0,
                        "vmaf_score": 94.0,
                        "ok": True,
                        "error": "",
                    },
                    {
                        "codec": "libx265",
                        "encoder_version": "n3.5",
                        "best_crf": 30,
                        "bitrate_kbps": 3200.0,
                        "encode_time_ms": 200.0,
                        "vmaf_score": 93.0,
                        "ok": True,
                        "error": "",
                    },
                    {
                        "codec": "libsvtav1",
                        "encoder_version": "",
                        "best_crf": -1,
                        "bitrate_kbps": None,
                        "encode_time_ms": None,
                        "vmaf_score": None,
                        "ok": False,
                        "error": "encoder unavailable (libsvtav1): Encoder not found",
                    },
                ]
            }
        )
    )
    output = tmp_path / "card.md"

    rc = cli_main(
        [
            "report",
            "--src",
            str(src),
            "--compare-json",
            str(compare_json),
            "--output",
            str(output),
            "--format",
            "markdown",
            "--target-vmaf",
            "93",
        ]
    )
    out = capsys.readouterr().out
    assert rc == 0, out
    payload = json.loads(out.strip().splitlines()[-1])
    assert payload["ok"] is True, payload
    assert payload["degraded"] is True, payload
    assert payload["codec_rows"] == 3
    assert payload["codec_rows_ok"] == 2
    assert payload["codec_rows_failed"] == 1
    assert payload["codec_rows_unavailable"] == 1


def test_report_real_encode_failure_flips_ok_false(tmp_path: Path, capsys: Any) -> None:
    """A non-"encoder unavailable" failure must still flip ``ok=false``.

    Anti-regression guard for the V4-C fix: don't let
    "encoder unavailable" coverage accidentally swallow real
    bisect-side failures too.
    """
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * (1920 * 1080 * 3 // 2))
    compare_json = tmp_path / "compare.json"
    compare_json.write_text(
        json.dumps(
            {
                "rows": [
                    {
                        "codec": "libx264",
                        "encoder_version": "n6.0",
                        "best_crf": 26,
                        "bitrate_kbps": 4000.0,
                        "encode_time_ms": 100.0,
                        "vmaf_score": 94.0,
                        "ok": True,
                        "error": "",
                    },
                    {
                        "codec": "libx265",
                        "encoder_version": "n3.5",
                        "best_crf": -1,
                        "bitrate_kbps": None,
                        "encode_time_ms": None,
                        "vmaf_score": None,
                        "ok": False,
                        "error": "bisect bounds did not converge",
                    },
                ]
            }
        )
    )
    rc = cli_main(
        [
            "report",
            "--src",
            str(src),
            "--compare-json",
            str(compare_json),
            "--output",
            str(tmp_path / "card.md"),
            "--format",
            "markdown",
            "--target-vmaf",
            "93",
        ]
    )
    out = capsys.readouterr().out
    assert rc == 0, out
    payload = json.loads(out.strip().splitlines()[-1])
    assert payload["ok"] is False, payload
    assert payload["degraded"] is False, payload
    assert payload["codec_rows_unavailable"] == 0


def test_report_all_unavailable_keeps_ok_false_when_no_row_succeeds(
    tmp_path: Path, capsys: Any
) -> None:
    """If every row is unavailable and none succeed, ``ok=false`` is correct.

    The rule "unavailable rows don't gate ok" only applies when at
    least one row actually succeeded. An all-unavailable run produced
    no quality data — the report is informational only and should
    not be marked ``ok=true``.
    """
    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * (1920 * 1080 * 3 // 2))
    compare_json = tmp_path / "compare.json"
    compare_json.write_text(
        json.dumps(
            {
                "rows": [
                    {
                        "codec": "libsvtav1",
                        "encoder_version": "",
                        "best_crf": -1,
                        "ok": False,
                        "error": "encoder unavailable (libsvtav1): Encoder not found",
                    },
                ]
            }
        )
    )
    rc = cli_main(
        [
            "report",
            "--src",
            str(src),
            "--compare-json",
            str(compare_json),
            "--output",
            str(tmp_path / "card.md"),
            "--format",
            "markdown",
            "--target-vmaf",
            "93",
        ]
    )
    out = capsys.readouterr().out
    assert rc == 0, out
    payload = json.loads(out.strip().splitlines()[-1])
    assert payload["ok"] is False, payload
    assert payload["degraded"] is True, payload
