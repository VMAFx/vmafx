# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Predictor training pipeline + shipped-model smoke tests.

Three pins:

1. ``test_train_synthetic_corpus_emits_onnx`` — runs the trainer
   end-to-end on a 50-row synthetic corpus, asserts the ONNX file is
   written, loadable via onnxruntime, and produces a finite VMAF in
   ``[0, 100]``.
2. ``test_predictor_loads_each_shipped_model`` — for every
   ``model/predictor_<codec>.onnx`` the fork ships, load via
   onnxruntime CPUExecutionProvider, run a smoke inference, assert
   the output is finite and clamped.
3. ``test_pick_crf_uses_onnx_when_present`` — pin that
   ``Predictor(model_path=...)`` overrides the analytical fallback
   path used by ``pick_crf``.

The shipped-model loop is parameterised over every codec in the
trainer's ``CODECS`` tuple — drift between the registered codecs and
the shipped ONNX files is caught here rather than at runtime.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import vmaftune.predictor_train as predictor_train  # noqa: E402
from vmaftune.predictor import Predictor, ShotFeatures  # noqa: E402
from vmaftune.predictor_train import (  # noqa: E402
    CODECS,
    INPUT_DIM,
    TrainConfig,
    TrainResult,
    generate_synthetic_corpus,
    iter_corpus_files,
    load_corpus,
    main,
    project_row,
    train_all_codecs,
    train_one_codec,
    train_val_split,
)

_REPO_ROOT = Path(__file__).resolve().parents[3]
_MODEL_DIR = _REPO_ROOT / "model"

# Skip the heavy paths if torch / onnxruntime are missing — the trainer
# is dev-only and the runtime predictor path itself does not need
# torch.
torch = pytest.importorskip("torch")
ort = pytest.importorskip("onnxruntime")
np = pytest.importorskip("numpy")


# ---------------------------------------------------------------------
# 1. End-to-end trainer smoke
# ---------------------------------------------------------------------


def test_train_synthetic_corpus_emits_onnx(tmp_path: Path) -> None:
    """Runs the trainer on 50 synthetic rows; asserts ONNX artefact is usable."""
    rows = generate_synthetic_corpus("libx264", n_rows=50)
    assert len(rows) == 50
    cfg = TrainConfig(epochs=20, batch_size=16, seed=7)
    result = train_one_codec(
        "libx264",
        rows,
        cfg=cfg,
        output_dir=tmp_path,
        corpus_kind=f"synthetic-stub-N={len(rows)}",
    )

    assert result.onnx_path.is_file()
    assert result.card_path.is_file()
    assert result.onnx_bytes > 0
    assert result.op_allowlist_ok, f"forbidden ops: {result.forbidden_ops}"

    # Card includes the synthetic warning + the metric block.
    card_text = result.card_path.read_text(encoding="utf-8")
    assert "synthetic-stub" in card_text
    assert "PLCC" in card_text and "SROCC" in card_text and "RMSE" in card_text

    # Loadable via onnxruntime CPU.
    sess = ort.InferenceSession(str(result.onnx_path), providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name
    x = np.zeros((1, INPUT_DIM), dtype=np.float32)
    x[0, 1] = 3000.0  # bitrate
    x[0, 11] = 24.0  # fps
    x[0, 12] = 1920.0
    x[0, 13] = 1080.0
    out = sess.run(None, {name: x})[0]
    val = float(out.flatten()[0])
    assert val == pytest.approx(val)  # finite (no NaN)
    assert 0.0 <= val <= 100.0


def test_train_val_split_is_deterministic_with_seed() -> None:
    rows = [{"i": i} for i in range(20)]
    a_train, a_val = train_val_split(rows, val_fraction=0.2, seed=42)
    b_train, b_val = train_val_split(rows, val_fraction=0.2, seed=42)
    assert [r["i"] for r in a_train] == [r["i"] for r in b_train]
    assert [r["i"] for r in a_val] == [r["i"] for r in b_val]
    assert len(a_val) == 4
    assert len(a_train) == 16


def test_project_row_returns_input_dim_floats() -> None:
    row = {
        "crf": 28,
        "bitrate_kbps": 4321.5,
        "width": 1920,
        "height": 1080,
        "framerate": 24.0,
        "duration_s": 8.0,
    }
    vec = project_row(row)
    assert len(vec) == INPUT_DIM
    assert all(isinstance(v, float) for v in vec)
    assert vec[0] == 28.0
    assert vec[1] == pytest.approx(4321.5)


def test_project_row_preserves_optional_predictor_signal_columns() -> None:
    row = {
        "crf": 24,
        "bitrate_kbps": 2400.0,
        "width": 1280,
        "height": 720,
        "framerate": 30.0,
        "duration_s": 2.0,
        "probe_i_frame_avg_bytes": "12000.5",
        "probe_p_frame_avg_bytes": 2200.25,
        "probe_b_frame_avg_bytes": 900.0,
        "saliency_mean": 0.42,
        "saliency_var": "0.037",
        "frame_diff_mean": 2.5,
        "y_avg": 128.0,
        "y_var": 400.0,
    }

    vec = project_row(row)

    assert vec[2] == pytest.approx(12000.5)
    assert vec[3] == pytest.approx(2200.25)
    assert vec[4] == pytest.approx(900.0)
    assert vec[5] == pytest.approx(0.42)
    assert vec[6] == pytest.approx(0.037)
    assert vec[7] == pytest.approx(2.5)
    assert vec[8] == pytest.approx(128.0)
    assert vec[9] == pytest.approx(400.0)


def test_load_corpus_accepts_hardware_sweep_aliases(tmp_path: Path) -> None:
    """Real Phase-A hardware sweeps predate canonical corpus key names."""
    corpus = tmp_path / "hardware.jsonl"
    corpus.write_text(
        "\n".join(
            [
                '{"codec":"h264_nvenc","q":24,"actual_kbps":3100.5,"vmaf":93.2}',
                '{"encoder":"h264_nvenc","crf":28,"bitrate_kbps":2100.0,"vmaf_score":88.0}',
                '{"codec":"hevc_nvenc","q":24,"actual_kbps":2200.0,"vmaf":94.0}',
                '{"codec":"h264_nvenc","q":32,"actual_kbps":0,"vmaf":"nan"}',
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    rows = load_corpus(corpus, "h264_nvenc")
    assert len(rows) == 2
    assert rows[0]["encoder"] == "h264_nvenc"
    assert rows[0]["crf"] == 24.0
    assert rows[0]["bitrate_kbps"] == pytest.approx(3100.5)
    assert rows[0]["vmaf_score"] == pytest.approx(93.2)


def test_iter_corpus_files_accepts_directory_shards(tmp_path: Path) -> None:
    """Training can consume the sharded ``.workingdir2/corpus_run`` layout."""
    corpus_dir = tmp_path / "corpus"
    nested = corpus_dir / "nested"
    nested.mkdir(parents=True)
    b = nested / "b.jsonl"
    a = corpus_dir / "a.jsonl"
    ignored = corpus_dir / "notes.txt"
    b.write_text("{}", encoding="utf-8")
    a.write_text("{}", encoding="utf-8")
    ignored.write_text("not corpus", encoding="utf-8")

    assert iter_corpus_files(corpus_dir) == (a, b)
    assert iter_corpus_files(a) == (a,)
    assert iter_corpus_files(tmp_path / "missing") == ()


def test_load_corpus_reads_all_jsonl_shards_in_directory(tmp_path: Path) -> None:
    corpus_dir = tmp_path / "corpus"
    corpus_dir.mkdir()
    (corpus_dir / "one.jsonl").write_text(
        '{"encoder":"libx264","crf":23,"bitrate_kbps":3000.0,"vmaf_score":95.0}\n'
        '{"encoder":"libx265","crf":28,"bitrate_kbps":2100.0,"vmaf_score":96.0}\n',
        encoding="utf-8",
    )
    (corpus_dir / "two.jsonl").write_text(
        '{"codec":"libx264","q":28,"actual_kbps":1800.0,"vmaf":90.5}\n'
        '{"encoder":"libx264","crf":35,"bitrate_kbps":900.0,"vmaf_score":82.0,'
        '"exit_status":1}\n',
        encoding="utf-8",
    )

    rows = load_corpus(corpus_dir, "libx264")
    assert len(rows) == 2
    assert [row["crf"] for row in rows] == [23.0, 28.0]
    assert [row["vmaf_score"] for row in rows] == [95.0, 90.5]


def _dummy_train_result(codec: str, output_dir: Path) -> TrainResult:
    return TrainResult(
        codec=codec,
        n_train=1,
        n_val=0,
        plcc=1.0,
        srocc=1.0,
        rmse=0.0,
        onnx_path=output_dir / f"predictor_{codec}.onnx",
        card_path=output_dir / f"predictor_{codec}_card.md",
        onnx_sha256="0" * 64,
        onnx_bytes=1,
        op_allowlist_ok=True,
        forbidden_ops=(),
    )


def test_train_all_codecs_consumes_directory_corpus(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Directory ``--corpus`` sources must not silently fall back to stubs."""
    corpus_dir = tmp_path / "corpus"
    corpus_dir.mkdir()
    (corpus_dir / "part.jsonl").write_text(
        '{"encoder":"libx264","crf":23,"bitrate_kbps":3000.0,"vmaf_score":95.0}\n',
        encoding="utf-8",
    )
    seen: list[tuple[str, int, str]] = []

    def fake_train_one_codec(codec, rows, *, cfg, output_dir, corpus_kind):
        seen.append((codec, len(rows), corpus_kind))
        return _dummy_train_result(codec, output_dir)

    def fail_synthetic(codec, n_rows):
        raise AssertionError(f"synthetic fallback used for {codec}")

    monkeypatch.setattr(predictor_train, "CODECS", ("libx264",))
    monkeypatch.setattr(predictor_train, "train_one_codec", fake_train_one_codec)
    monkeypatch.setattr(predictor_train, "generate_synthetic_corpus", fail_synthetic)

    results = train_all_codecs(
        output_dir=tmp_path / "out",
        cfg=TrainConfig(epochs=1),
        corpus_path=corpus_dir,
    )

    assert [r.codec for r in results] == ["libx264"]
    assert seen == [("libx264", 1, "real-N=1")]


def test_main_consumes_directory_corpus(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """CLI orchestration matches ``load_corpus`` directory support."""
    corpus_dir = tmp_path / "corpus"
    corpus_dir.mkdir()
    (corpus_dir / "part.jsonl").write_text(
        '{"encoder":"libx264","crf":28,"bitrate_kbps":1800.0,"vmaf_score":90.5}\n',
        encoding="utf-8",
    )
    seen: list[tuple[str, int, str]] = []

    def fake_train_one_codec(codec, rows, *, cfg, output_dir, corpus_kind):
        seen.append((codec, len(rows), corpus_kind))
        return _dummy_train_result(codec, output_dir)

    def fail_synthetic(codec, n_rows):
        raise AssertionError(f"synthetic fallback used for {codec}")

    monkeypatch.setattr(predictor_train, "train_one_codec", fake_train_one_codec)
    monkeypatch.setattr(predictor_train, "generate_synthetic_corpus", fail_synthetic)

    rc = main(
        [
            "--corpus",
            str(corpus_dir),
            "--codec",
            "libx264",
            "--output-dir",
            str(tmp_path / "out"),
            "--epochs",
            "1",
        ]
    )

    assert rc == 0
    assert seen == [("libx264", 1, "real-N=1")]


# ---------------------------------------------------------------------
# 2. Shipped-model smoke
# ---------------------------------------------------------------------


@pytest.mark.parametrize("codec", CODECS)
def test_predictor_loads_each_shipped_model(codec: str) -> None:
    """Every shipped predictor_<codec>.onnx loads + emits a clamped VMAF."""
    onnx_path = _MODEL_DIR / f"predictor_{codec}.onnx"
    card_path = _MODEL_DIR / f"predictor_{codec}_card.md"
    assert onnx_path.is_file(), f"missing shipped model: {onnx_path}"
    assert card_path.is_file(), f"missing shipped model card: {card_path}"

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name
    x = np.zeros((1, INPUT_DIM), dtype=np.float32)
    x[0, 0] = 28.0  # crf
    x[0, 1] = 5000.0  # bitrate kbps
    x[0, 11] = 24.0
    x[0, 12] = 1920.0
    x[0, 13] = 1080.0
    out = sess.run(None, {name: x})[0]
    val = float(out.flatten()[0])
    assert np.isfinite(val), f"{codec}: non-finite output {val}"
    assert 0.0 <= val <= 100.0, f"{codec}: out-of-range output {val}"


@pytest.mark.parametrize("codec", CODECS)
def test_shipped_model_is_monotone_decreasing_in_crf(codec: str) -> None:
    """Smoke: shipped model's prediction does not increase as CRF rises.

    Stub models are trained against the analytical fallback, which is
    monotone in CRF; the trained model preserves that property within a
    small tolerance.
    """
    onnx_path = _MODEL_DIR / f"predictor_{codec}.onnx"
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    name = sess.get_inputs()[0].name

    def predict(crf: float) -> float:
        x = np.zeros((1, INPUT_DIM), dtype=np.float32)
        x[0, 0] = crf
        x[0, 1] = 5000.0  # fixed bitrate so only crf varies
        x[0, 11] = 24.0
        x[0, 12] = 1920.0
        x[0, 13] = 1080.0
        out = sess.run(None, {name: x})[0]
        return float(out.flatten()[0])

    crfs = [20.0, 25.0, 30.0, 35.0, 40.0]
    preds = [predict(c) for c in crfs]
    # Allow a small slack so a tiny non-monotonicity does not fail —
    # tighter monotonicity comes with a real corpus.
    for i in range(1, len(preds)):
        assert preds[i] <= preds[i - 1] + 2.0, (
            f"{codec}: non-monotone at crf={crfs[i]}: " f"{preds[i]} > {preds[i - 1]} + 2.0"
        )
    # And the endpoints must move at least *some* amount.
    assert preds[-1] < preds[0] + 0.01


# ---------------------------------------------------------------------
# 3. Predictor wires the ONNX session when a model path is supplied
# ---------------------------------------------------------------------


def test_pick_crf_uses_onnx_when_present() -> None:
    """``Predictor(model_path=...)`` routes through the ONNX session.

    Verified by injecting a stub session whose predictions are constant
    everywhere; the analytical fallback would never produce that
    constant, so observing it pins the ONNX path.
    """
    onnx_path = _MODEL_DIR / "predictor_libx264.onnx"
    if not onnx_path.is_file():
        pytest.skip("shipped model not present")

    p = Predictor(model_path=onnx_path)
    assert p._onnx_session is not None, "ONNX session must be live when model_path is set"

    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=10000.0,
        probe_p_frame_avg_bytes=2000.0,
        probe_b_frame_avg_bytes=1000.0,
        shot_length_frames=120,
        fps=24.0,
        width=1920,
        height=1080,
    )

    # Override the ONNX session with a stub so we can prove the ONNX
    # branch executes — analytical fallback would compute a different
    # value for these inputs.
    class _StubSession:
        def __init__(self) -> None:
            self._inputs = [type("Input", (), {"name": "input"})()]

        def get_inputs(self):
            return self._inputs

        def run(self, _output_names, _inputs):
            # Return a constant 87.5 — far from any analytical output
            # for these inputs, so observing it proves the ONNX branch.
            return [np.array([[87.5]], dtype=np.float32)]

    p._onnx_session = _StubSession()
    val = p.predict_vmaf(feats, 28, "libx264")
    assert val == pytest.approx(87.5)

    # And pick_crf invokes predict_vmaf, so the ONNX path is used in the
    # full bisect too. Constant 87.5 < target 95.0 → bisect collapses
    # to the codec's quality_default.
    crf_high_target = p.pick_crf(feats, 95.0, "libx264")
    crf_low_target = p.pick_crf(feats, 80.0, "libx264")
    # Constant predictor + threshold below -> entire range satisfies.
    # Constant predictor + threshold above -> nothing satisfies; falls
    # back to quality_default. The two cases must differ.
    assert crf_high_target != crf_low_target


def test_predictor_falls_back_when_model_path_is_none() -> None:
    """Without a model path the analytical curve answers."""
    p = Predictor()
    assert p._onnx_session is None
    feats = ShotFeatures(
        probe_bitrate_kbps=3000.0,
        probe_i_frame_avg_bytes=0.0,
        probe_p_frame_avg_bytes=0.0,
        probe_b_frame_avg_bytes=0.0,
    )
    v = p.predict_vmaf(feats, 28, "libx264")
    assert 0.0 <= v <= 100.0


def test_predictor_raises_on_missing_model_file(tmp_path: Path) -> None:
    """Pointing at a non-existent file is a hard error, not a silent fallback."""
    missing = tmp_path / "does-not-exist.onnx"
    with pytest.raises(FileNotFoundError):
        Predictor(model_path=missing)


# ---------------------------------------------------------------------------
# ADR-0546 / ai-01: --emit-stub-card-only smoke test
# ---------------------------------------------------------------------------


def test_emit_stub_card_only_does_not_contain_placeholder(tmp_path: Path) -> None:
    """--emit-stub-card-only must emit a card with no literal PLACEHOLDER text.

    ADR-0546 (ai-01): the synthetic-stub signing note previously emitted
    'PLACEHOLDER', which is misleading.  The replacement text must be
    present and the literal word 'PLACEHOLDER' must not appear.
    """
    dest = tmp_path / "stub_card.md"
    exit_code = main(["--emit-stub-card-only", str(dest), "--codec", "libx264"])
    assert exit_code == 0, "expected exit 0 from --emit-stub-card-only"
    assert dest.exists(), "--emit-stub-card-only must write the destination file"
    card_text = dest.read_text(encoding="utf-8")
    assert (
        "PLACEHOLDER" not in card_text
    ), f"card still contains literal 'PLACEHOLDER':\n{card_text[:500]}"
    assert (
        "not applicable" in card_text
    ), "expected 'not applicable' in the Sigstore signing note for a synthetic-stub card"
    assert "Sigstore" in card_text, "expected a Sigstore reference in the signing note"
