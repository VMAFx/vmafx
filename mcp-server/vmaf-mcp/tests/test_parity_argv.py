# Copyright 2026 Lusoris. All rights reserved.
# Use of this source code is governed by the BSD-3-Clause-Plus-Patent
# license that can be found in the LICENSE file.

from pathlib import Path

from vmaf_mcp import server as srv


def test_build_vmaf_argv_minimal_fr() -> None:
    req = srv.ScoreRequest(
        ref=Path("/data/ref.yuv"),
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        model="version=vmaf_v0.6.1",
        backend="auto",
        precision="legacy",
        output_fmt="json",
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.json")
    expected = [
        "vmaf",
        "-r",
        "/data/ref.yuv",
        "-d",
        "/data/dis.yuv",
        "--width",
        "1920",
        "--height",
        "1080",
        "-p",
        "420",
        "-b",
        "8",
        "-m",
        "version=vmaf_v0.6.1",
        "--precision",
        "legacy",
        "-q",
        "-o",
        "/tmp/out.json",
        "--json",
    ]
    assert argv == expected


def test_build_vmaf_argv_output_formats() -> None:
    for fmt, flag in [("json", "--json"), ("xml", "--xml"), ("csv", "--csv"), ("sub", "--sub")]:
        req = srv.ScoreRequest(
            ref=Path("/data/ref.yuv"),
            dis=Path("/data/dis.yuv"),
            width=1920,
            height=1080,
            pixfmt="420",
            bitdepth=8,
            output_fmt=fmt,
        )
        argv = srv._build_vmaf_argv(req, vmaf="vmaf", output=f"/tmp/out.{fmt}")
        assert flag in argv
        assert f"/tmp/out.{fmt}" in argv


def test_build_vmaf_argv_backend_disables() -> None:
    req = srv.ScoreRequest(
        ref=Path("/data/ref.yuv"),
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        backend="cpu",
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.json")
    assert argv[-4:] == ["--no_cuda", "--no_sycl", "--no_hip", "--no_metal"]


def test_build_vmaf_argv_device_selectors() -> None:
    extras = srv._extras_from_args(
        {
            "cpumask": 7,
            "gpumask": 1,
            "sycl_device": 0,
            "hip_device": 1,
            "metal_device": 2,
        }
    )
    req = srv.ScoreRequest(
        ref=Path("/data/ref.yuv"),
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        extras=extras,
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.json")
    joined = " ".join(argv)
    assert "--cpumask 7" in joined
    assert "--gpumask 1" in joined
    assert "--sycl_device 0" in joined
    assert "--hip_device 1" in joined
    assert "--metal_device 2" in joined


def test_build_vmaf_argv_tiny_ai_full() -> None:
    extras = srv._extras_from_args(
        {
            "tiny_model": "/m/nr.onnx",
            "tiny_device": "cuda",
            "tiny_threads": 4,
            "tiny_fp16": True,
            "tiny_model_verify": True,
            "tiny_codec": "libx264",
            "tiny_preset": "medium",
            "tiny_crf": 23,
            "tiny_resize": "bilinear",
            "no_reference": True,
        }
    )
    req = srv.ScoreRequest(
        ref=None,
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        backend="cpu",
        extras=extras,
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.json")
    assert "-r" not in argv
    joined = " ".join(argv)
    assert "--tiny-model /m/nr.onnx" in joined
    assert "--tiny-device cuda" in joined
    assert "--tiny-threads 4" in joined
    assert "--tiny-fp16" in joined
    assert "--tiny-model-verify" in joined
    assert "--tiny-codec libx264" in joined
    assert "--tiny-preset medium" in joined
    assert "--tiny-crf 23" in joined
    assert "--tiny-resize bilinear" in joined
    assert "--no-reference" in joined


def test_build_vmaf_argv_subsample_and_features() -> None:
    extras = srv._extras_from_args(
        {
            "feature": ["psnr", "cambi=full_ref=true"],
            "aom_ctc": "v3.0",
            "threads": 8,
            "frame_cnt": 100,
            "frame_skip_ref": 2,
            "frame_skip_dist": 0,
            "no_prediction": True,
        }
    )
    req = srv.ScoreRequest(
        ref=Path("/data/ref.yuv"),
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        subsample=3,
        extras=extras,
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.json")
    joined = " ".join(argv)
    assert "--subsample 3" in joined
    assert "--feature psnr" in joined
    assert "--frame_skip_dist 0" in joined
    assert "--no_prediction" in joined
    # Subsample must precede feature
    assert joined.index("--subsample 3") < joined.index("--feature psnr")


def test_build_vmaf_argv_model_clip_transform_and_csv_sub() -> None:
    extras = srv._extras_from_args(
        {
            "disable_clip": True,
            "enable_transform": True,
            "csv": True,
        }
    )
    req = srv.ScoreRequest(
        ref=Path("/data/ref.yuv"),
        dis=Path("/data/dis.yuv"),
        width=1920,
        height=1080,
        pixfmt="420",
        bitdepth=8,
        model="version=vmaf_v0.6.1",
        output_fmt=extras.output_fmt,
        extras=extras,
    )
    argv = srv._build_vmaf_argv(req, vmaf="vmaf", output="/tmp/out.csv")
    idx = argv.index("-m")
    assert argv[idx + 1] == "version=vmaf_v0.6.1:disable_clip:enable_transform"
    assert "--csv" in argv
