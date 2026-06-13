"""End-to-end: YUV → libvmaf features → ONNX FR model → score.

Stitches the pieces that normally live in different phases of the
training/deploy pipeline and proves they actually compose:

  1. the vmaf CLI is driven to produce per-frame feature JSON,
  2. `feature_dump` parses the JSON (handling the `integer_` prefix
     libvmaf emits for fixed-point kernels),
  3. a tiny linear FR ONNX model is consumed by onnxruntime on those
     feature rows and yields finite per-frame scores.

This is the test that would have caught the two bugs we just fixed
in `feature_dump.py` — wrong pix_fmt default and missing
`integer_<name>` fallback. A CI run against the Netflix golden YUV
pair is the cheapest place to keep that guarantee.

The test is skipped if either the vmaf binary under `core/build-cpu/tools/`
or the Netflix YUV fixtures are missing, so it no-ops on a fresh
checkout without a build.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

onnx = pytest.importorskip("onnx")
ort = pytest.importorskip("onnxruntime")

import numpy as np  # noqa: E402
from onnx import TensorProto, helper  # noqa: E402

from vmaf_train.data.feature_dump import DEFAULT_FEATURES, Entry, dump_features  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
# Honour VMAF_BIN so any worktree / CI run can point at a freshly-built binary.
# Default follows the post-ADR-0700 rename: libvmaf/ → core/.
# Use None as sentinel: Path('') == Path('.'), which would execute CWD as binary.
_vmaf_bin_env = os.environ.get("VMAF_BIN")  # None when unset; VMAF_BIN='' means unset
VMAF_BIN = (
    Path(_vmaf_bin_env) if _vmaf_bin_env else (REPO_ROOT / "core" / "build-cpu" / "tools" / "vmaf")
)
# Honour VMAF_YUVDIR for worktrees where python/test/resource/ isn't checked out.
YUV_DIR = Path(
    os.environ.get(
        "VMAF_YUVDIR",
        str(REPO_ROOT / "python" / "test" / "resource" / "yuv"),
    )
)
REF_YUV = YUV_DIR / "src01_hrc00_576x324.yuv"
DIS_YUV = YUV_DIR / "src01_hrc01_576x324.yuv"


def _make_linear_fr_onnx(path: Path, n_features: int) -> None:
    """Tiny linear FR model: score = features · W + b. Matches FRRegressor's
    export shape (N, F) → (N,) so the pipeline is representative of what
    a trained C1 model ships."""
    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", n_features])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N"])
    w = helper.make_tensor(
        "W",
        TensorProto.FLOAT,
        [n_features, 1],
        np.full((n_features, 1), 1.0 / n_features, dtype=np.float32).flatten().tolist(),
    )
    b = helper.make_tensor("b", TensorProto.FLOAT, [1], [0.0])
    matmul = helper.make_node("MatMul", ["features", "W"], ["mm"])
    add = helper.make_node("Add", ["mm", "b"], ["wide"])
    # MatMul yields (N, 1); squeeze to (N,) so the output shape matches
    # the FRRegressor default (non-variance) forward().
    axes = helper.make_tensor("axes", TensorProto.INT64, [1], [1])
    squeeze = helper.make_node("Squeeze", ["wide", "axes"], ["score"])
    graph = helper.make_graph(
        [matmul, add, squeeze],
        "fr_linear",
        [x],
        [y],
        [w, b, axes],
    )
    onnx.save(
        helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]),
        str(path),
    )


def test_frame_to_score_pipeline(tmp_path: Path) -> None:
    if not VMAF_BIN.exists():
        pytest.skip(f"vmaf binary not built at {VMAF_BIN}")
    if not REF_YUV.exists() or not DIS_YUV.exists():
        pytest.skip("Netflix YUV fixtures missing")

    entry = Entry(
        key="src01_hrc01",
        ref=REF_YUV,
        dis=DIS_YUV,
        width=576,
        height=324,
        pix_fmt="yuv420p",  # exercise FFmpeg→vmaf pix_fmt translation
    )
    parquet = tmp_path / "features.parquet"
    dump_features([entry], parquet, vmaf_binary=VMAF_BIN, features=DEFAULT_FEATURES)
    assert parquet.exists()

    import pandas as pd

    df = pd.read_parquet(parquet)
    assert len(df) > 0
    # Every feature must be populated — this is the regression test for
    # the `integer_` prefix lookup fix.
    for f in DEFAULT_FEATURES:
        assert f in df.columns
        assert df[f].notna().all(), f"feature {f} has nulls — integer_ lookup broken?"

    feat_matrix = df[list(DEFAULT_FEATURES)].to_numpy(dtype=np.float32)
    assert np.isfinite(feat_matrix).all()

    model_path = tmp_path / "fr_tiny.onnx"
    _make_linear_fr_onnx(model_path, n_features=len(DEFAULT_FEATURES))
    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    scores = sess.run(None, {"features": feat_matrix})[0]

    assert scores.shape == (len(df),)
    assert np.isfinite(scores).all()
    # Sanity: our weights average the six features, all of which sit in
    # [0, ~5] for this clip, so scores must land in a sensible band.
    assert scores.min() >= 0.0 and scores.max() < 10.0
