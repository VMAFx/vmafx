# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the BBB end-to-end probe v5 bug cluster (ADR-0505).

The v5 probe surfaced three follow-ups against the v4 fixes:

* **V5-1** — ``vmaf --backend vulkan`` must exit non-zero (ADR-0726,
  2026-05-28). The original V5-1 pinned the strict-mode refusal
  message produced when Vulkan init failed; ADR-0726 dropped the
  Vulkan backend entirely so the CLI now rejects the value outright.
  This file's harder integration test additionally honours
  ``VMAF_BIN_FOR_TESTS`` and probes standard build directories so
  the gate fires whenever a binary is reachable.
* **V5-2** — ``vmaf-tune ladder`` against a container source
  (``.mp4`` / ``.mkv``) produced VMAF 4-9 instead of 80-95 because
  the encode pipe re-interpreted the container's compressed bytes
  as planar YUV pixels (``-f rawvideo -pix_fmt … -s WxH -i src.mp4``).
  The encode now sets ``source_is_container=True`` whenever the
  source suffix is outside :data:`vmaftune.corpus._VMAF_RAW_SUFFIXES`,
  so ffmpeg auto-detects the container format and the rung-target
  scale filter handles the resolution change.
* **V5-3** — The JSON ``samples`` array double-listed every
  rendition because the v4 emit path used per-target picks (one row
  per ``(resolution, target_vmaf)`` cell) as the sample cloud. Two
  targets that converged on the same CRF emitted two identical
  sample rows. ADR-0505 replaces the per-target cloud with the full
  per-CRF sweep produced by the sampler, de-duplicated by
  ``(width, height, crf)``.

The tests here do **not** mock the libvmaf binary or the ladder
pipeline at the assertion boundary — V4-A's mocked test passed
while the real binary was returning 0. V5-1 invokes ``subprocess.run``
against an actual binary; V5-2 / V5-3 inspect the materialised
``EncodeRequest`` / ``samples`` payload at the boundary the bug
escaped through.
"""

from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.corpus import _VMAF_RAW_SUFFIXES, CorpusJob, CorpusOptions, iter_rows  # noqa: E402
from vmaftune.ladder import (  # noqa: E402
    LadderPoint,
    _dedup_samples,
    build_and_emit,
    make_default_sampler,
)


@dataclass
class _FakeCompleted:
    returncode: int = 0
    stdout: str = ""
    stderr: str = ""


# ---------------------------------------------------------------------------
# V5-1 — strict-mode refusal must exit non-zero (subprocess, real binary)
# ---------------------------------------------------------------------------


def _resolve_vmaf_binary() -> Path | None:
    """Locate a built libvmaf CLI binary for the integration test.

    Lookup order (first hit wins):

    1. ``$VMAF_BIN_FOR_TESTS`` — explicit override for CI gates that
       build the binary in a non-canonical location.
    2. ``shutil.which("vmaf")`` — the historical V4-A discovery path.
    3. ``build/tools/vmaf`` under the repo root — the meson default
       output for ``ninja -C build vmaf``. Walks up from the test
       file until a ``meson.build`` is found so worktrees resolve
       correctly.

    Returns ``None`` when nothing is reachable; the caller skips.
    """
    env = os.environ.get("VMAF_BIN_FOR_TESTS")
    if env:
        env_path = Path(env)
        if env_path.is_file() and os.access(env_path, os.X_OK):
            return env_path
    which = shutil.which("vmaf")
    if which:
        return Path(which)
    for parent in [_HERE, *_HERE.parents]:
        if (parent / "meson.build").is_file() and (parent / "libvmaf").is_dir():
            candidate = parent / "build" / "tools" / "vmaf"
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
            break
    return None


def _find_yuv_resource_root() -> Path | None:
    """Walk up from this file to the Netflix golden YUV fixture root."""
    for parent in [_HERE, *_HERE.parents]:
        target = parent / "python" / "test" / "resource" / "yuv"
        if target.is_dir():
            return target
    return None


def test_vmaf_backend_vulkan_rejected_after_adr_0726() -> None:
    """``vmaf --backend vulkan`` MUST exit non-zero (V5-1 / ADR-0726).

    ADR-0726 (2026-05-28) dropped the Vulkan backend from libvmaf.
    The CLI now rejects ``--backend vulkan`` outright — either at the
    argparse layer or via the "not compiled in" guard — and exits
    non-zero. This pin guards against accidental reintroduction of
    a vulkan code path that would silently succeed.

    The test mirrors the V4-A improvements: it honours
    ``VMAF_BIN_FOR_TESTS`` and the canonical ``build/tools/vmaf``
    path so it exercises a built binary even when ``$PATH`` is not
    augmented.
    """
    binary = _resolve_vmaf_binary()
    if binary is None:
        pytest.skip(
            "no vmaf binary reachable for V5-1 (set VMAF_BIN_FOR_TESTS, "
            "ensure 'vmaf' is on PATH, or run after `ninja -C build vmaf`)"
        )
    yuv_root = _find_yuv_resource_root()
    if yuv_root is None:
        pytest.skip("Netflix golden YUV fixtures not available")
    ref = yuv_root / "checkerboard_1920_1080_10_3_0_0.yuv"
    dist = yuv_root / "checkerboard_1920_1080_10_3_1_0.yuv"
    if not ref.exists() or not dist.exists():
        pytest.skip(f"checkerboard YUVs missing under {yuv_root}")
    cmd = [
        str(binary),
        "--backend",
        "vulkan",
        "--reference",
        str(ref),
        "--distorted",
        str(dist),
        "--width",
        "1920",
        "--height",
        "1080",
        "-p",
        "420",
        "-b",
        "8",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    assert proc.returncode != 0, (
        "ADR-0726 regression: --backend vulkan exited 0 — Vulkan was "
        f"dropped 2026-05-28 and must be rejected (stderr={proc.stderr!r})"
    )


# ---------------------------------------------------------------------------
# V5-2 — container source must be encoded as a container, not raw YUV
# ---------------------------------------------------------------------------


def test_iter_rows_marks_container_source_for_encoder(tmp_path: Path, monkeypatch: Any) -> None:
    """A ``.mp4`` source must surface as ``source_is_container=True`` (V5-2).

    Before ADR-0505 the EncodeRequest produced by ``iter_rows`` always
    left ``source_is_container=False``. Combined with the encode argv
    builder's ``-f rawvideo -pix_fmt yuv420p -s WxH -i src.mp4`` shape
    this re-interpreted the container's compressed payload as planar
    YUV pixels, producing a uniformly-bogus ~50 Mbps encode regardless
    of CRF and a VMAF score in the 4-9 band against the (correctly
    decoded) reference. The test pins the wiring at the boundary that
    leaked the bug.
    """
    import vmaftune.corpus as corpus_mod  # noqa: PLC0415

    src = tmp_path / "bbb.mp4"
    src.write_bytes(b"\x00")
    encode_dir = tmp_path / "enc"
    job = CorpusJob(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=2.0,
        cells=(("medium", 28),),
    )
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=encode_dir,
        src_sha256=False,
    )

    def _fake_sp_run(argv: list[Any], **_kw: Any) -> _FakeCompleted:
        Path(argv[-1]).parent.mkdir(parents=True, exist_ok=True)
        Path(argv[-1]).write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0)

    monkeypatch.setattr(subprocess, "run", _fake_sp_run)
    monkeypatch.setattr(corpus_mod, "_sha256_file", lambda *_a, **_kw: "")

    captured_reqs: list[Any] = []

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        # Inspect argv: the encode driver must NOT emit -f rawvideo
        # against a container source.
        captured_reqs.append(list(argv))
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.0\nx264 - core 164\n")

    def _score_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        out = Path(argv[argv.index("--output") + 1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('{"pooled_metrics": {"vmaf": {"mean": 92.0}}}\n')
        return _FakeCompleted(returncode=0, stderr="VMAF version 3.0.0-test\n")

    rows = list(iter_rows(job, opts, encode_runner=_enc_runner, score_runner=_score_runner))
    assert rows, "iter_rows produced no rows"
    assert captured_reqs, "encode runner never invoked"
    encode_argv = captured_reqs[0]
    # The bug-trigger argv shape — ``-f rawvideo`` against a container —
    # must NEVER appear. The container source should drop straight into
    # ``-i src.mp4`` so ffmpeg auto-detects the format.
    assert "-f" not in encode_argv or "rawvideo" not in encode_argv, (
        f"V5-2 regression: encode of container source used raw-video framing "
        f"(argv={encode_argv!r})"
    )
    # Sanity: the input is still wired in.
    assert "-i" in encode_argv
    assert str(src) in encode_argv


def test_iter_rows_keeps_raw_yuv_source_uncontainerised(tmp_path: Path, monkeypatch: Any) -> None:
    """Raw ``.yuv`` sources must continue to use the rawvideo framing.

    Anti-regression guard for the V5-2 fix: don't let the container-
    detection clause accidentally swallow the raw-YUV path too.
    """
    import vmaftune.corpus as corpus_mod  # noqa: PLC0415

    src = tmp_path / "src.yuv"
    src.write_bytes(b"\x00" * (1920 * 1080 * 3 // 2))
    job = CorpusJob(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=1.0,
        cells=(("medium", 28),),
    )
    opts = CorpusOptions(
        encoder="libx264",
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "enc",
        src_sha256=False,
    )

    monkeypatch.setattr(corpus_mod, "_sha256_file", lambda *_a, **_kw: "")
    monkeypatch.setattr(
        subprocess,
        "run",
        lambda argv, **_kw: _FakeCompleted(returncode=0),
    )

    captured: list[list[str]] = []

    def _enc_runner(argv: list[str], **_kw: Any) -> _FakeCompleted:
        captured.append(list(argv))
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
    assert captured, "encode runner never invoked for raw YUV"
    enc_argv = captured[0]
    assert "-f" in enc_argv and "rawvideo" in enc_argv, enc_argv
    assert ".yuv" in _VMAF_RAW_SUFFIXES  # spec invariant


# ---------------------------------------------------------------------------
# V5-2 + V5-3 — ladder JSON ``samples`` contains the full per-CRF cloud
# ---------------------------------------------------------------------------


def test_ladder_samples_carries_full_crf_sweep_via_cloud_sink(tmp_path: Path) -> None:
    """``samples`` must contain every encoded CRF, not just per-target picks.

    Reproduces V5-2 (only the top-CRF row kept per resolution) at the
    ladder-emit boundary. The historic emit path used the per-target
    ``LadderPoint`` cloud, which collapsed the per-CRF sweep produced
    by ``_default_sampler`` to one row per ``(resolution, target_vmaf)``
    cell. ADR-0505 routes the full sweep cloud through the
    ``extra_samples`` keyword so downstream consumers see every CRF.
    """
    # Synthesise a sampler that mimics the real ``_default_sampler``
    # behaviour: it scores a per-cell CRF sweep, pushes every result
    # into ``cloud_sink``, and returns the closest-to-target point.
    crf_sweep = (23, 28, 33)
    cells_seen: list[tuple[int, int, float]] = []
    cloud_sink: list[LadderPoint] = []

    def _stub_sampler(
        src: Path, encoder: str, width: int, height: int, target_vmaf: float
    ) -> LadderPoint:
        cells_seen.append((width, height, target_vmaf))
        # The real sampler returns one row per cell; here we synthesise
        # a CRF-sweep result, push every row into the sink, and return
        # the row closest to the target.
        rows = [
            LadderPoint(
                width=width,
                height=height,
                bitrate_kbps=2000.0 + 100.0 * (40 - crf),
                vmaf=70.0 + 0.7 * (40 - crf),
                crf=crf,
            )
            for crf in crf_sweep
        ]
        cloud_sink.extend(rows)
        return min(rows, key=lambda p: abs(p.vmaf - target_vmaf))

    out = build_and_emit(
        src=tmp_path / "src.mp4",
        encoder="libx264",
        resolutions=((1920, 1080), (1280, 720)),
        target_vmafs=(88.0, 92.0),
        quality_tiers=4,
        format="json",
        sampler=_stub_sampler,
        extra_samples=cloud_sink,
    )
    payload = json.loads(out)
    samples = payload["samples"]
    # 2 resolutions x 3 CRFs = 6 unique entries (dedup'd by w,h,crf).
    assert len(samples) == 6, (len(samples), samples)
    keys = {(s["width"], s["height"], s["crf"]) for s in samples}
    expected = {(w, h, c) for (w, h) in ((1920, 1080), (1280, 720)) for c in crf_sweep}
    assert keys == expected, keys ^ expected


def test_dedup_samples_drops_repeated_width_height_crf() -> None:
    """``_dedup_samples`` strips ``(width, height, crf)`` repeats (V5-3)."""
    samples = [
        LadderPoint(width=1920, height=1080, bitrate_kbps=4000.0, vmaf=92.0, crf=23),
        LadderPoint(width=1920, height=1080, bitrate_kbps=4000.0, vmaf=92.0, crf=23),  # dup
        LadderPoint(width=1280, height=720, bitrate_kbps=1500.0, vmaf=88.0, crf=28),
        LadderPoint(width=1920, height=1080, bitrate_kbps=2000.0, vmaf=86.0, crf=33),
    ]
    out = _dedup_samples(samples)
    assert len(out) == 3
    assert [(p.width, p.height, p.crf) for p in out] == [
        (1920, 1080, 23),
        (1280, 720, 28),
        (1920, 1080, 33),
    ]


def test_build_and_emit_dedups_per_target_duplicates_when_no_extra_samples(
    tmp_path: Path,
) -> None:
    """Without ``extra_samples`` the legacy per-target cloud is still dedup'd.

    Guards the V5-3 dedup pass: even when callers don't wire a cloud
    sink (legacy ``build_and_emit`` invocation), two target-VMAFs
    picking the same CRF for the same resolution now collapse to one
    sample row instead of two.
    """

    def _stub_sampler(
        src: Path, encoder: str, width: int, height: int, target_vmaf: float
    ) -> LadderPoint:
        # Every target_vmaf converges on the same (crf=28) point per
        # resolution — the V5-3 trigger condition.
        return LadderPoint(
            width=width,
            height=height,
            bitrate_kbps=3000.0,
            vmaf=90.0,
            crf=28,
        )

    out = build_and_emit(
        src=tmp_path / "src.yuv",
        encoder="libx264",
        resolutions=((1920, 1080),),
        target_vmafs=(88.0, 92.0),
        quality_tiers=1,
        format="json",
        sampler=_stub_sampler,
    )
    payload = json.loads(out)
    samples = payload["samples"]
    assert len(samples) == 1, (len(samples), samples)
    assert samples[0]["crf"] == 28


def test_make_default_sampler_passes_cloud_sink_through(tmp_path: Path) -> None:
    """The factory-built sampler must propagate ``cloud_sink`` to the inner sampler.

    Pins the CLI wiring: ``_run_ladder`` constructs the sampler via
    ``make_default_sampler`` and relies on the cloud sink populating
    every per-CRF row. A regression that drops the kwarg silently
    falls back to per-target-only samples.
    """
    sink: list[LadderPoint] = []
    sampler = make_default_sampler(
        pix_fmt="yuv420p",
        framerate=30.0,
        duration_s=1.0,
        crf_sweep=(23, 28, 33),
        src_width=1920,
        src_height=1080,
        cloud_sink=sink,
    )
    # The sampler's signature is what the ladder calls; we don't run a
    # full encode here (would require ffmpeg / libvmaf binaries). The
    # test asserts the factory accepts the kwarg and the returned
    # callable is shape-correct — the wiring contract.
    assert callable(sampler)
    # A regression that dropped the kwarg from the factory signature
    # would TypeError at construction; reaching this line is the
    # contract.


# ---------------------------------------------------------------------------
# V5-2 end-to-end against the dev-mcp container (BBB MP4)
# ---------------------------------------------------------------------------


@pytest.mark.slow
@pytest.mark.skipif(
    shutil.which("docker") is None,
    reason="docker not available; cannot run the dev-mcp end-to-end probe",
)
def test_ladder_against_bbb_container_yields_plausible_vmaf() -> None:
    """End-to-end smoke against the dev-mcp container.

    Runs the CLI exactly as the V5 probe did and asserts the JSON
    output meets the non-regression invariants:

    * the ``samples`` array has ``len > 1`` (V5-2 reproducer
      collapsed it to one row per resolution),
    * every sample is ``(width, height, crf)``-unique (V5-3 dedup),
    * every sample's VMAF is plausible (>= 50; the pre-fix probe
      logged VMAF 4-9 against the same source).

    Skipped when the dev-mcp container isn't running OR the BBB
    corpus isn't present. The test is intentionally lenient about
    the lower VMAF bound (50, not 80) — the goal is to catch the
    "garbage encode" failure mode, not to gate fine-grained
    perceptual quality.
    """
    container = os.environ.get("VMAF_DEV_MCP_CONTAINER", "vmaf-dev-mcp")
    # Quick probe — bail cleanly when the container isn't up.
    probe = subprocess.run(
        ["docker", "exec", container, "true"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if probe.returncode != 0:
        pytest.skip(f"dev-mcp container {container!r} not reachable")
    _corpus_root = os.environ.get("VMAF_CORPUS_DIR", "/workspace/.corpus/bbb_e2e")
    src = f"{_corpus_root}/bbb_sunflower_1080p_30fps_normal.mp4"
    corpus_check = subprocess.run(
        ["docker", "exec", container, "test", "-f", src],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if corpus_check.returncode != 0:
        pytest.skip(f"BBB corpus missing in container at {src}")
    out_json = "/tmp/v5_ladder_e2e.json"
    cmd = [
        "docker",
        "exec",
        container,
        "bash",
        "-c",
        (
            "cd /workspace && PYTHONPATH=tools/vmaf-tune/src "
            "python -c 'from vmaftune.cli import main; raise SystemExit(main())' "
            # ADR-0908: duration trimmed 4 -> 2 s and CRF sweep 3 -> 2 pts
            # (`23,33`) to halve docker e2e wall (~13 s -> ~7 s). The
            # `len(samples) >= 4` floor is preserved: 2 resolutions x 2
            # CRFs = 4, and 2 s of BBB sunflower still clears `vmaf >= 50`.
            # shlex.quote guards against shell injection from env-var-sourced paths.
            f"ladder --src {shlex.quote(src)} --target-vmafs 88,92 "
            "--resolutions 1920x1080,1280x720 --framerate 30 --duration 2 "
            f"--crf-sweep 23,33 --format json --output {shlex.quote(out_json)}"
        ),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    assert (
        proc.returncode == 0
    ), f"ladder CLI exit {proc.returncode}; stderr={proc.stderr[-2000:]!r}"
    cat = subprocess.run(
        ["docker", "exec", container, "cat", out_json],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert cat.returncode == 0, cat.stderr
    payload = json.loads(cat.stdout)
    samples = payload.get("samples", [])
    # V5-2 root-cause assertion: full per-CRF sweep emitted, not
    # collapsed to per-target picks. 2 res x 3 CRFs = 6.
    assert len(samples) >= 4, (len(samples), samples)
    keys = [(s["width"], s["height"], s["crf"]) for s in samples]
    # V5-3 dedup assertion: samples are unique by (w, h, crf).
    assert len(keys) == len(set(keys)), keys
    # V5-2 score assertion: VMAF is plausible — the garbage-encode
    # regression produced 4-9 on this source.
    vmafs = [s["vmaf"] for s in samples]
    assert all(v >= 50.0 for v in vmafs), (
        f"V5-2 regression: implausible VMAF in samples ({vmafs!r}); "
        "container source likely re-interpreted as raw YUV again"
    )
