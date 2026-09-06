# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""CLI surface tests for ``vmaf-tune fast`` (HP-3, ADR-0276 + ADR-0304).

Validates three things:

1.  argparse wiring — every documented flag is reachable via ``--help``
    and the subparser accepts the expected types.
2.  Smoke-mode end-to-end — ``--smoke`` runs the synthetic curve through
    Optuna and emits a JSON payload with the canonical
    ``recommend`` / ``predict`` schema (single source of truth per the
    HP-3 audit).
3.  Production-mode wiring — when callers do not pass ``--smoke`` the
    CLI builds the canonical-6 sample extractor and verify runner from
    the existing :mod:`vmaftune.encode` + :mod:`vmaftune.score` pipeline
    rather than raising ``NotImplementedError``. Subprocess calls are
    monkey-patched so the suite never spawns ffmpeg or vmaf.

The tests stay deliberately light on infrastructure mocking — the
fast-path module's contract tests in ``test_fast.py`` cover the TPE +
proxy + verify seams. This file's job is the CLI surface.
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

# Make src/ importable without an editable install — mirrors test_fast.py.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

optuna = pytest.importorskip("optuna")

from vmaftune import cli as cli_module
from vmaftune.cli import main

# Documented flags the audit's HP-3 surface promises. Keep this list in
# sync with ``_add_fast_args`` and the ``## fast`` section of
# ``docs/usage/vmaf-tune.md`` — drift here is the user-discoverable bug
# this test exists to catch.
_DOCUMENTED_FAST_FLAGS: tuple[str, ...] = (
    "--src",
    "--width",
    "--height",
    "--pix-fmt",
    "--framerate",
    "--target-vmaf",
    "--encoder",
    "--preset",
    "--crf-min",
    "--crf-max",
    "--n-trials",
    "--time-budget-s",
    "--proxy-tolerance",
    "--sample-chunk-seconds",
    "--smoke",
    "--score-backend",
    "--ffmpeg-bin",
    "--vmaf-bin",
    "--vmaf-model",
    "--encode-dir",
    "--output",
)


def _capture_help(argv: list[str]) -> str:
    """Run ``main(argv)`` expecting a SystemExit and capture stdout."""
    buf = io.StringIO()
    with patch("sys.stdout", buf), pytest.raises(SystemExit) as exc:
        main(argv)
    assert exc.value.code == 0
    return buf.getvalue()


def test_fast_subparser_is_registered() -> None:
    """``vmaf-tune fast`` resolves to a subparser in the top-level CLI."""
    parser = cli_module._build_parser()
    # argparse's subparsers action lives at the action with `choices`.
    sub_actions = [a for a in parser._actions if hasattr(a, "choices") and a.choices]
    fast_choices = {
        name for action in sub_actions for name in (action.choices or {}) if name == "fast"
    }
    assert "fast" in fast_choices


def test_fast_help_lists_every_documented_flag() -> None:
    """``vmaf-tune fast --help`` advertises the full HP-3 surface."""
    help_text = _capture_help(["fast", "--help"])
    missing = [flag for flag in _DOCUMENTED_FAST_FLAGS if flag not in help_text]
    assert not missing, f"--help is missing documented flags: {missing}"


def test_fast_smoke_emits_recommend_schema_payload(capsys: pytest.CaptureFixture) -> None:
    """End-to-end smoke run produces the canonical recommend/predict schema."""
    rc = main(
        [
            "fast",
            "--target-vmaf",
            "92.0",
            "--smoke",
            "--n-trials",
            "12",
        ]
    )
    assert rc == 0
    out = capsys.readouterr().out
    payload = json.loads(out)
    # Same JSON shape downstream consumers (recommend / predict) expect:
    # encoder + target + recommended_crf + predicted_vmaf + predicted_kbps,
    # plus the fast-path-specific verify diagnostics.
    expected_keys = {
        "encoder",
        "target_vmaf",
        "recommended_crf",
        "predicted_vmaf",
        "predicted_kbps",
        "n_trials",
        "smoke",
        "notes",
        "verify_vmaf",
        "proxy_verify_gap",
    }
    assert expected_keys.issubset(payload.keys())
    assert payload["smoke"] is True
    assert payload["target_vmaf"] == pytest.approx(92.0)
    assert payload["encoder"] == "libx264"
    assert 10 <= payload["recommended_crf"] <= 51
    # Smoke never runs the verify pass.
    assert payload["verify_vmaf"] is None
    assert payload["proxy_verify_gap"] is None


def test_fast_smoke_writes_output_file(tmp_path: Path) -> None:
    """``--output PATH`` writes the JSON payload to disk and stays silent on stdout."""
    out_path = tmp_path / "rec.json"
    rc = main(
        [
            "fast",
            "--target-vmaf",
            "85.0",
            "--smoke",
            "--n-trials",
            "8",
            "--output",
            str(out_path),
        ]
    )
    assert rc == 0
    assert out_path.exists()
    payload = json.loads(out_path.read_text(encoding="utf-8"))
    assert payload["smoke"] is True
    assert payload["target_vmaf"] == pytest.approx(85.0)


def test_fast_production_mode_requires_src(capsys: pytest.CaptureFixture) -> None:
    """Without ``--smoke`` the CLI demands a source path before doing any work."""
    rc = main(["fast", "--target-vmaf", "92.0"])
    assert rc == 2
    err = capsys.readouterr().err
    assert "--src" in err


def test_fast_production_mode_requires_geometry(
    tmp_path: Path, capsys: pytest.CaptureFixture
) -> None:
    """Without ``--width`` / ``--height`` the CLI errors out early."""
    fake_src = tmp_path / "src.yuv"
    fake_src.write_bytes(b"")
    rc = main(["fast", "--target-vmaf", "92.0", "--src", str(fake_src)])
    assert rc == 2
    err = capsys.readouterr().err
    assert "--width" in err or "--height" in err


def test_fast_production_invokes_extractor_and_runner(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture,
) -> None:
    """Production CLI builds the seams and threads them into ``fast_recommend``.

    We monkey-patch the seam factories to record-only stubs so the
    test stays in-process. The CLI's job is the wiring; the seam
    contracts themselves are exercised in ``test_fast.py``.
    """
    captured: dict[str, object] = {}

    def _fake_extractor_factory(args, workdir):
        captured["extractor_workdir"] = workdir
        captured["extractor_args"] = args

        def _ext(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
            captured.setdefault("extractor_calls", []).append((str(src), crf, encoder))
            return ([0.5, 0.4, 0.3, 0.2, 0.1, 0.05], 1500.0)

        return _ext

    def _fake_runner_factory(args, workdir, backend):
        captured["runner_workdir"] = workdir
        captured["runner_backend"] = backend

        def _runner(src: Path, encoder: str, crf: int, advisory: str) -> tuple[float, float]:
            captured.setdefault("runner_calls", []).append((str(src), encoder, crf, advisory))
            # Return a verify VMAF close to whatever the proxy curve
            # would have produced at this CRF, so the proxy/verify gap
            # stays inside the default 1.5 tolerance.
            crf_norm = (crf - 10) / max(51 - 10, 1)
            return (1500.0, 100.0 - 30.0 * crf_norm + 0.2)

        return _runner

    def _fake_select_backend(prefer: str = "auto", vmaf_bin: str = "vmaf") -> str:
        captured["selected_backend"] = prefer
        return "cpu"

    monkeypatch.setattr(cli_module, "_build_fast_sample_extractor", _fake_extractor_factory)
    monkeypatch.setattr(cli_module, "_build_fast_encode_runner", _fake_runner_factory)
    monkeypatch.setattr(cli_module, "select_backend", _fake_select_backend)

    # Stub the v2 proxy session — the CLI test must not require the
    # actual fr_regressor_v2 ONNX file or onnxruntime to be available.
    import vmaftune.proxy as proxy_module

    def _fake_run_proxy(features, *, encoder, preset_norm, crf_norm, **_):
        # Deterministic CRF -> predicted VMAF curve so TPE has signal.
        return 100.0 - 30.0 * crf_norm

    monkeypatch.setattr(proxy_module, "run_proxy", _fake_run_proxy)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"")
    encode_dir = tmp_path / "scratch"

    rc = main(
        [
            "fast",
            "--src",
            str(src),
            "--width",
            "1920",
            "--height",
            "1080",
            "--target-vmaf",
            "92.0",
            "--n-trials",
            "5",
            "--encode-dir",
            str(encode_dir),
        ]
    )
    # Verify is constructed to land within tolerance of the synthetic
    # proxy curve at the recommended CRF.
    assert rc == 0
    out = capsys.readouterr().out
    payload = json.loads(out)
    assert payload["smoke"] is False
    assert payload["score_backend"] == "cpu"
    assert payload["verify_vmaf"] is not None
    assert payload["proxy_verify_gap"] is not None
    assert payload["proxy_verify_gap"] < 1.5
    # The CLI built and used both seams.
    assert captured.get("extractor_workdir") == encode_dir / "probes"
    assert captured.get("runner_workdir") == encode_dir / "verify"
    assert captured.get("runner_backend") == "cpu"
    # Verify pass invoked exactly once at the end.
    assert len(captured.get("runner_calls", [])) == 1
    # Extractor ran at least one TPE trial.
    assert len(captured.get("extractor_calls", [])) >= 1


def test_fast_ood_gap_returns_nonzero(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """When verify disagrees with proxy past tolerance the CLI exits non-zero.

    Operators chain ``vmaf-tune fast || vmaf-tune recommend``; the
    non-zero exit is the fall-back-to-slow-grid signal documented in
    ADR-0276.
    """

    def _fake_extractor_factory(args, workdir):
        def _ext(src: Path, crf: int, encoder: str) -> tuple[list[float], float]:
            return ([0.5, 0.4, 0.3, 0.2, 0.1, 0.05], 2000.0)

        return _ext

    def _fake_runner_factory(args, workdir, backend):
        def _runner(src: Path, encoder: str, crf: int, advisory: str) -> tuple[float, float]:
            # Verify disagrees by 5 VMAF points — way past tolerance.
            return (2000.0, 87.0)

        return _runner

    monkeypatch.setattr(cli_module, "_build_fast_sample_extractor", _fake_extractor_factory)
    monkeypatch.setattr(cli_module, "_build_fast_encode_runner", _fake_runner_factory)
    monkeypatch.setattr(cli_module, "select_backend", lambda prefer="auto", vmaf_bin="vmaf": "cpu")

    import vmaftune.proxy as proxy_module

    def _fake_run_proxy(features, *, encoder, preset_norm, crf_norm, **_):
        # Synthetic monotone proxy: predicts 92 at crf_norm 0.27 (CRF ~24).
        return 100.0 - 30.0 * crf_norm

    monkeypatch.setattr(proxy_module, "run_proxy", _fake_run_proxy)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"")

    rc = main(
        [
            "fast",
            "--src",
            str(src),
            "--width",
            "1280",
            "--height",
            "720",
            "--target-vmaf",
            "92.0",
            "--n-trials",
            "4",
            "--proxy-tolerance",
            "1.5",
            "--encode-dir",
            str(tmp_path / "scratch"),
            "--output",
            str(tmp_path / "result.json"),
        ]
    )
    # 3 = OOD signal so a wrapper can fall through to ``recommend``.
    assert rc == 3
    payload = json.loads((tmp_path / "result.json").read_text(encoding="utf-8"))
    assert payload["proxy_verify_gap"] > 1.5
    assert "FLAG" in payload["notes"]


def test_fast_invalid_crf_range_errors() -> None:
    """``--crf-min`` greater than ``--crf-max`` is an early argparse-side reject."""
    rc = main(
        [
            "fast",
            "--target-vmaf",
            "92.0",
            "--smoke",
            "--crf-min",
            "30",
            "--crf-max",
            "20",
        ]
    )
    assert rc == 2


def test_parse_canonical6_means_pooled_path() -> None:
    """Pooled-metrics shape returns the per-feature means in canonical order."""
    payload = {
        "pooled_metrics": {
            "adm2": {"mean": 0.95},
            "vif_scale0": {"mean": 0.80},
            "vif_scale1": {"mean": 0.85},
            "vif_scale2": {"mean": 0.88},
            "vif_scale3": {"mean": 0.90},
            "motion2": {"mean": 1.20},
        }
    }
    means = cli_module._parse_canonical6_means(payload)
    assert means == [0.95, 0.80, 0.85, 0.88, 0.90, 1.20]


def test_parse_canonical6_means_per_frame_fallback() -> None:
    """Per-frame metrics fall back to a mean when pooled is absent."""
    payload = {
        "frames": [
            {"metrics": {"adm2": 0.9, "vif_scale0": 0.8}},
            {"metrics": {"adm2": 0.8, "vif_scale0": 0.7}},
        ]
    }
    means = cli_module._parse_canonical6_means(payload)
    # adm2 mean = 0.85, vif_scale0 = 0.75, rest absent -> 0.0.
    assert means[0] == pytest.approx(0.85)
    assert means[1] == pytest.approx(0.75)
    assert means[2:] == [0.0, 0.0, 0.0, 0.0]


def test_parse_canonical6_means_integer_pipeline_keys() -> None:
    """Modern libvmaf integer_* pooled keys are resolved in canonical order."""
    payload = {
        "pooled_metrics": {
            "integer_adm2": {"mean": 0.98},
            "integer_vif_scale0": {"mean": 0.82},
            "integer_vif_scale1": {"mean": 0.86},
            "integer_vif_scale2": {"mean": 0.89},
            "integer_vif_scale3": {"mean": 0.92},
            "integer_motion2": {"mean": 4.50},
        }
    }
    means = cli_module._parse_canonical6_means(payload)
    assert means == [0.98, 0.82, 0.86, 0.89, 0.92, 4.50]


def test_parse_canonical6_means_normalise_flag() -> None:
    """normalise=True normalises the raw features using sidecar stats."""
    from vmaftune.proxy import load_proxy_sidecar

    sidecar = load_proxy_sidecar()
    mean = sidecar["feature_mean"]
    std = sidecar["feature_std"]
    # Feature vector exactly equal to mean -> normalised must be all zeros
    payload = {
        "pooled_metrics": {
            "integer_adm2": {"mean": mean[0]},
            "integer_vif_scale0": {"mean": mean[1]},
            "integer_vif_scale1": {"mean": mean[2]},
            "integer_vif_scale2": {"mean": mean[3]},
            "integer_vif_scale3": {"mean": mean[4]},
            "integer_motion2": {"mean": mean[5]},
        }
    }
    norm_means = cli_module._parse_canonical6_means(payload, normalise=True)
    for v in norm_means:
        assert v == pytest.approx(0.0, abs=1e-6)

    # Feature vector equal to mean + std -> normalised must be all ones
    payload_plus_std = {
        "pooled_metrics": {
            "integer_adm2": {"mean": mean[0] + std[0]},
            "integer_vif_scale0": {"mean": mean[1] + std[1]},
            "integer_vif_scale1": {"mean": mean[2] + std[2]},
            "integer_vif_scale2": {"mean": mean[3] + std[3]},
            "integer_vif_scale3": {"mean": mean[4] + std[4]},
            "integer_motion2": {"mean": mean[5] + std[5]},
        }
    }
    norm_means_std = cli_module._parse_canonical6_means(payload_plus_std, normalise=True)
    for v in norm_means_std:
        assert v == pytest.approx(1.0, abs=1e-6)


def test_parse_canonical6_means_per_frame_integer_fallback() -> None:
    """Per-frame integer_* metrics fall back to a mean when pooled is absent."""
    payload = {
        "frames": [
            {"metrics": {"integer_adm2": 0.9, "integer_vif_scale0": 0.8}},
            {"metrics": {"integer_adm2": 0.8, "integer_vif_scale0": 0.7}},
        ]
    }
    means = cli_module._parse_canonical6_means(payload)
    assert means[0] == pytest.approx(0.85)
    assert means[1] == pytest.approx(0.75)
    assert means[2:] == [0.0, 0.0, 0.0, 0.0]


def test_build_fast_sample_extractor_decodes_distorted_mp4_and_cleans_up(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """_build_fast_sample_extractor decodes container to raw YUV before vmaf."""
    import argparse
    from unittest.mock import MagicMock

    import vmaftune.cli as cli_mod
    from vmaftune.encode import EncodeResult

    args = argparse.Namespace(
        width=576,
        height=324,
        pix_fmt="yuv420p",
        framerate=24.0,
        preset="medium",
        sample_chunk_seconds=2.0,
        ffmpeg_bin="ffmpeg",
        vmaf_bin="vmaf",
        vmaf_model="version=vmaf_v0.6.1",
    )
    workdir = tmp_path / "probes"

    def _fake_run_encode(req, ffmpeg_bin="ffmpeg"):
        req.output.write_bytes(b"dummy mp4 container")
        return EncodeResult(
            request=req,
            exit_status=0,
            encode_time_ms=10.0,
            encode_size_bytes=5000,
            encoder_version="libx264-enabled",
            ffmpeg_version="n9.0.1",
            stderr_tail="",
        )

    monkeypatch.setattr("vmaftune.encode.run_encode", _fake_run_encode)

    decoded_file = tmp_path / "decoded_probe.yuv"
    decode_called = []

    def _fake_maybe_decode_distorted(score_req, workdir, ffmpeg_bin="ffmpeg"):
        decode_called.append(score_req.distorted)
        decoded_file.write_bytes(b"dummy raw yuv")
        import dataclasses

        return dataclasses.replace(score_req, distorted=decoded_file), 0

    monkeypatch.setattr("vmaftune.score.maybe_decode_distorted", _fake_maybe_decode_distorted)

    vmaf_commands = []

    def _fake_build_vmaf_command(score_req, json_path, vmaf_bin="vmaf", backend=None):
        vmaf_commands.append(score_req.distorted)
        return ["echo", "vmaf", "--output", str(json_path)]

    monkeypatch.setattr("vmaftune.score.build_vmaf_command", _fake_build_vmaf_command)

    def _sub_run_writing_json(cmd, capture_output=True, text=True, check=False):
        if "--output" in cmd:
            out_idx = cmd.index("--output") + 1
            out_path = Path(cmd[out_idx])
            payload = {
                "pooled_metrics": {
                    "integer_adm2": {"mean": 0.9550306797027588},
                    "integer_vif_scale0": {"mean": 0.5178422331809998},
                    "integer_vif_scale1": {"mean": 0.8622183203697205},
                    "integer_vif_scale2": {"mean": 0.9155327677726746},
                    "integer_vif_scale3": {"mean": 0.9446256160736084},
                    "integer_motion2": {"mean": 8.946569442749023},
                }
            }
            out_path.write_text(json.dumps(payload), encoding="utf-8")
        return MagicMock(returncode=0, stdout="", stderr="")

    monkeypatch.setattr("subprocess.run", _sub_run_writing_json)

    extractor = cli_mod._build_fast_sample_extractor(args, workdir)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"")
    features, kbps = extractor(src, 28, "libx264")

    # Verify decode was called with probe .mp4
    assert len(decode_called) == 1
    assert decode_called[0].name == "probe_libx264_crf28.mp4"

    # Verify build_vmaf_command received the decoded raw YUV, not .mp4
    assert len(vmaf_commands) == 1
    assert vmaf_commands[0] == decoded_file

    # Verify decoded file was cleaned up in finally
    assert not decoded_file.exists()

    # Verify features are normalised (since raw matched mean, normalised should be ~0.0)
    for f in features:
        assert f == pytest.approx(0.0, abs=1e-6)
    assert kbps > 0.0


def test_build_fast_sample_extractor_raises_on_nonzero_vmaf_exit(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """_build_fast_sample_extractor raises RuntimeError on non-zero vmaf exit."""
    import argparse
    from unittest.mock import MagicMock

    import vmaftune.cli as cli_mod
    from vmaftune.encode import EncodeResult

    args = argparse.Namespace(
        width=576,
        height=324,
        pix_fmt="yuv420p",
        framerate=24.0,
        preset="medium",
        sample_chunk_seconds=2.0,
        ffmpeg_bin="ffmpeg",
        vmaf_bin="vmaf",
        vmaf_model="version=vmaf_v0.6.1",
    )
    workdir = tmp_path / "probes"

    def _fake_run_encode(req, ffmpeg_bin="ffmpeg"):
        req.output.write_bytes(b"dummy mp4")
        return EncodeResult(
            request=req,
            exit_status=0,
            encode_time_ms=10.0,
            encode_size_bytes=5000,
            encoder_version="libx264-enabled",
            ffmpeg_version="n9.0.1",
            stderr_tail="",
        )

    monkeypatch.setattr("vmaftune.encode.run_encode", _fake_run_encode)

    def _fake_maybe_decode_distorted(score_req, workdir, ffmpeg_bin="ffmpeg"):
        decoded_file = tmp_path / "decoded.yuv"
        decoded_file.write_bytes(b"dummy yuv")
        import dataclasses

        return dataclasses.replace(score_req, distorted=decoded_file), 0

    monkeypatch.setattr("vmaftune.score.maybe_decode_distorted", _fake_maybe_decode_distorted)

    def _fake_sub_run(cmd, capture_output=True, text=True, check=False):
        return MagicMock(returncode=255, stdout="", stderr="Error reading YUV frame data")

    monkeypatch.setattr("subprocess.run", _fake_sub_run)

    extractor = cli_mod._build_fast_sample_extractor(args, workdir)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"")
    with pytest.raises(RuntimeError, match="vmaf score failed.*rc=255"):
        extractor(src, 28, "libx264")


def test_build_fast_sample_extractor_raises_on_encode_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """_build_fast_sample_extractor raises RuntimeError on non-zero encode exit."""
    import argparse

    import vmaftune.cli as cli_mod
    from vmaftune.encode import EncodeResult

    args = argparse.Namespace(
        width=576,
        height=324,
        pix_fmt="yuv420p",
        framerate=24.0,
        preset="medium",
        sample_chunk_seconds=2.0,
        ffmpeg_bin="ffmpeg",
        vmaf_bin="vmaf",
        vmaf_model="version=vmaf_v0.6.1",
    )
    workdir = tmp_path / "probes"

    def _fake_run_encode(req, ffmpeg_bin="ffmpeg"):
        return EncodeResult(
            request=req,
            exit_status=1,
            encode_time_ms=0.0,
            encode_size_bytes=0,
            encoder_version="libx264-enabled",
            ffmpeg_version="n9.0.1",
            stderr_tail="Encoder error",
        )

    monkeypatch.setattr("vmaftune.encode.run_encode", _fake_run_encode)

    extractor = cli_mod._build_fast_sample_extractor(args, workdir)

    src = tmp_path / "ref.yuv"
    src.write_bytes(b"")
    with pytest.raises(RuntimeError, match="encode failed"):
        extractor(src, 28, "libx264")
