# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``ai/scripts/chug_to_corpus_jsonl.py``."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "chug_to_corpus_jsonl.py"
_FEATURE_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "chug_extract_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("chug_to_corpus_jsonl", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CHUG = _load_module()
_feature_spec = importlib.util.spec_from_file_location(
    "chug_extract_features", _FEATURE_SCRIPT_PATH
)
assert _feature_spec is not None and _feature_spec.loader is not None
CHUG_FEATURES = importlib.util.module_from_spec(_feature_spec)
sys.modules[_feature_spec.name] = CHUG_FEATURES
_feature_spec.loader.exec_module(CHUG_FEATURES)


def _make_manifest(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "Video,mos_j,sos_j,ref,name,bitladder,resolution,bitrate,orientation,framerate,content_name,height,width\nabc123,50.0,2.5,0,360p_0.2M_src.mp4,360p_0.2M_,360p,0.2M,Landscape,30.0,src.mp4,360,640\ndef456,75.0,1.5,1,1080p_5M_ref.mp4,1080p_5M_,1080p,5M,Portrait,29.97,ref.mp4,1080,1920"
        + "\n",
        encoding="utf-8",
    )


def test_parse_manifest_maps_chug_mos_to_one_to_five(tmp_path: Path) -> None:
    manifest = tmp_path / "manifest.csv"
    _make_manifest(manifest)

    rows = CHUG.parse_manifest_csv(manifest, min_rows=1)

    assert len(rows) == 2
    assert rows[0]["filename"] == "abc123.mp4"
    assert rows[0]["url"].endswith("/abc123.mp4")
    assert rows[0]["mos_raw_0_100"] == 50.0
    assert rows[0]["mos"] == 3.0
    assert rows[1]["chug_orientation"] == "Portrait"


class _FakeRunner:
    def __call__(self, cmd, **_kwargs):
        argv0 = Path(cmd[0]).name
        if argv0 == "curl":
            output_path = Path(cmd[cmd.index("--output") + 1])
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(b"fake mp4")
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")
        if argv0 == "ffprobe":
            payload = {
                "streams": [
                    {
                        "width": 640,
                        "height": 360,
                        "r_frame_rate": "30/1",
                        "avg_frame_rate": "30/1",
                        "duration": "10.0",
                        "pix_fmt": "yuv420p10le",
                        "codec_name": "hevc",
                    }
                ],
                "format": {"duration": "10.0"},
            }
            return subprocess.CompletedProcess(
                args=cmd,
                returncode=0,
                stdout=json.dumps(payload),
                stderr="",
            )
        raise AssertionError(f"unexpected command: {cmd}")


def test_run_writes_chug_jsonl_with_hdr_metadata(tmp_path: Path) -> None:
    chug_dir = tmp_path / "chug"
    manifest = chug_dir / "manifest.csv"
    output = chug_dir / "chug.jsonl"
    _make_manifest(manifest)

    stats = CHUG.run(
        chug_dir=chug_dir,
        output=output,
        min_csv_rows=1,
        max_rows=1,
        runner=_FakeRunner(),
        now_fn=lambda: "2026-05-14T00:00:00+00:00",
    )

    assert stats.written == 1
    row = json.loads(output.read_text(encoding="utf-8"))
    assert row["corpus"] == "chug"
    assert row["pix_fmt"] == "yuv420p10le"
    assert row["mos"] == 3.0
    assert row["mos_raw_0_100"] == 50.0
    assert row["chug_bitladder"] == "360p_0.2M_"


def test_main_writes_replay_manifest(tmp_path: Path, monkeypatch) -> None:
    output = tmp_path / "chug.jsonl"

    def fake_run(**kwargs):
        kwargs["output"].parent.mkdir(parents=True, exist_ok=True)
        kwargs["output"].write_text("{}\n", encoding="utf-8")
        stats = CHUG.RunStats()
        stats.written = 1
        return stats

    monkeypatch.setattr(CHUG, "run", fake_run)

    rc = CHUG.main(
        [
            "--chug-dir",
            str(tmp_path / "chug"),
            "--output",
            str(output),
            "--min-csv-rows",
            "1",
            "--max-rows",
            "1",
        ]
    )

    assert rc == 0
    manifest = json.loads(output.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == "chug-corpus-jsonl-manifest-v1"
    assert manifest["corpus"] == "chug"
    assert manifest["stats"]["written"] == 1
    assert manifest["config"]["max_rows"] == 1
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert manifest["run_provenance"]["args"]["output"] == str(output)


def _chug_row(
    *,
    src: str,
    content: str,
    is_ref: bool,
    width: int,
    height: int,
    sha: str,
) -> dict:
    return {
        "src": src,
        "src_sha256": sha,
        "width": width,
        "height": height,
        "mos": 5.0 if is_ref else 3.0,
        "chug_content_name": content,
        "chug_ref": 1 if is_ref else 0,
        "chug_bitrate_label": "ref" if is_ref else "0.2M",
        "chug_bitladder": "1080p_ref_" if is_ref else "360p_0.2M_",
        "chug_video_id": src.removesuffix(".mp4"),
    }


def test_chug_feature_pairing_uses_matching_reference(tmp_path: Path) -> None:
    clips_dir = tmp_path / "clips"
    rows = [
        _chug_row(
            src="dist.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=640,
            height=360,
            sha="d" * 64,
        ),
        _chug_row(
            src="ref.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="r" * 64,
        ),
    ]

    pairs = CHUG_FEATURES.build_feature_pairs(rows, clips_dir=clips_dir)

    assert len(pairs) == 1
    assert pairs[0].dis_path == clips_dir / "dist.mp4"
    assert pairs[0].ref_path == clips_dir / "ref.mp4"
    assert pairs[0].width == 1920
    assert pairs[0].height == 1080
    assert pairs[0].split in {"train", "val", "test"}
    assert pairs[0].split_key == "content-a.mp4"


def test_chug_content_split_keeps_ladder_rows_together(tmp_path: Path) -> None:
    clips_dir = tmp_path / "clips"
    rows = [
        _chug_row(
            src="a_low.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=640,
            height=360,
            sha="a" * 64,
        ),
        _chug_row(
            src="a_ref.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="b" * 64,
        ),
        _chug_row(
            src="b_low.mp4",
            content="content-b.mp4",
            is_ref=False,
            width=640,
            height=360,
            sha="c" * 64,
        ),
        _chug_row(
            src="b_ref.mp4",
            content="content-b.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="d" * 64,
        ),
    ]

    split_map = CHUG_FEATURES.build_content_split_map(rows, seed="stable")
    pairs = CHUG_FEATURES.build_feature_pairs(rows, clips_dir=clips_dir, split_map=split_map)

    assert {pair.split_key for pair in pairs} == {"content-a.mp4", "content-b.mp4"}
    for pair in pairs:
        assert pair.split == split_map[pair.split_key]


def test_chug_split_manifest_records_run_provenance(tmp_path: Path) -> None:
    output = tmp_path / "splits.json"
    rows = [
        _chug_row(
            src="a_ref.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="a" * 64,
        )
    ]
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": "ai/scripts/chug_extract_features.py", "exists": True},
        "argv": ["--split-manifest", str(output)],
    }

    payload = CHUG_FEATURES.write_split_manifest(
        rows,
        output=output,
        seed="stable",
        run_provenance=provenance,
    )
    disk_payload = json.loads(output.read_text(encoding="utf-8"))

    assert payload["run_provenance"] == provenance
    assert disk_payload["run_provenance"] == provenance
    assert disk_payload["n_contents"] == 1


class _FakeAuditRunner:
    def __call__(self, cmd, **_kwargs):
        src = Path(cmd[-1]).name
        if src == "bad.mp4":
            stream = {
                "width": 640,
                "height": 360,
                "pix_fmt": "yuv420p10le",
                "codec_name": "hevc",
                "color_transfer": "smpte2084",
                "color_primaries": "bt709",
                "color_space": "bt2020nc",
                "color_range": "tv",
            }
        else:
            stream = {
                "width": 1920,
                "height": 1080,
                "pix_fmt": "yuv420p10le",
                "codec_name": "hevc",
                "color_transfer": "smpte2084",
                "color_primaries": "bt2020",
                "color_space": "bt2020nc",
                "color_range": "tv",
            }
        return subprocess.CompletedProcess(
            args=cmd,
            returncode=0,
            stdout=json.dumps({"streams": [stream]}),
            stderr="",
        )


def test_chug_hdr_audit_flags_malformed_hdr_metadata(tmp_path: Path) -> None:
    clips_dir = tmp_path / "clips"
    clips_dir.mkdir()
    (clips_dir / "ok.mp4").write_bytes(b"ok")
    (clips_dir / "bad.mp4").write_bytes(b"bad")
    output = tmp_path / "audit.json"
    rows = [
        _chug_row(
            src="ok.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="e" * 64,
        ),
        _chug_row(
            src="bad.mp4",
            content="content-b.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="f" * 64,
        ),
    ]

    payload = CHUG_FEATURES.audit_chug_hdr_metadata(
        rows,
        clips_dir=clips_dir,
        output=output,
        runner=_FakeAuditRunner(),
        run_provenance={"schema": "ai-run-provenance-v1", "argv": ["--audit-output"]},
    )

    assert output.is_file()
    assert payload["probed"] == 2
    assert payload["transfer_counts"] == {"pq": 2}
    assert payload["pix_fmt_counts"] == {"yuv420p10le": 2}
    assert payload["malformed_hdr_rows"] == [
        {
            "src": "bad.mp4",
            "chug_content_name": "content-b.mp4",
            "split": CHUG_FEATURES.content_split_for("content-b.mp4"),
            "color_transfer": "smpte2084",
            "color_primaries": "bt709",
            "pix_fmt": "yuv420p10le",
        }
    ]
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert json.loads(output.read_text(encoding="utf-8"))["run_provenance"]["argv"] == [
        "--audit-output"
    ]


class _FakeFeatureRunner:
    def __call__(self, cmd, **_kwargs):
        argv0 = Path(cmd[0]).name
        if argv0 == "ffprobe":
            src = Path(cmd[-1]).name
            stream = {
                "width": 1920,
                "height": 1080,
                "pix_fmt": "yuv420p10le",
                "codec_name": "hevc",
                "color_transfer": "smpte2084" if src == "ref.mp4" else "arib-std-b67",
                "color_primaries": "bt2020",
                "color_space": "bt2020nc",
                "color_range": "tv",
                "side_data_list": [
                    {
                        "side_data_type": "Mastering display metadata",
                        "max_content": "1000",
                        "max_average": "400",
                    }
                ],
            }
            return subprocess.CompletedProcess(
                args=cmd,
                returncode=0,
                stdout=json.dumps({"streams": [stream]}),
                stderr="",
            )
        src = Path(cmd[cmd.index("-i") + 1]).name
        vf = str(cmd[cmd.index("-vf") + 1])
        scale = vf.split(",", maxsplit=1)[0].removeprefix("scale=")
        width_s, height_s = scale.split(":", maxsplit=2)[:2]
        width = int(width_s)
        height = int(height_s)
        output = Path(cmd[-1])
        output.parent.mkdir(parents=True, exist_ok=True)
        if src == "dist.mp4":
            y = np.indices((height, width)).sum(axis=0) % 2
            luma = (y * 1023).astype("<u2")
        else:
            luma = np.full((height, width), 512, dtype="<u2")
        chroma = np.full((height // 2, width // 2), 512, dtype="<u2")
        output.write_bytes(luma.tobytes() + chroma.tobytes() + chroma.tobytes())
        return subprocess.CompletedProcess(args=cmd, returncode=0)


def test_chug_feature_materialiser_writes_mean_features(tmp_path: Path) -> None:
    chug_jsonl = tmp_path / "chug.jsonl"
    output = tmp_path / "features.jsonl"
    clips_dir = tmp_path / "clips"
    clips_dir.mkdir()
    (clips_dir / "dist.mp4").write_bytes(b"dist")
    (clips_dir / "ref.mp4").write_bytes(b"ref")
    rows = [
        _chug_row(
            src="dist.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=32,
            height=18,
            sha="d" * 64,
        ),
        _chug_row(
            src="ref.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=32,
            height=18,
            sha="r" * 64,
        ),
    ]
    chug_jsonl.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")

    def fake_extract(*_args, **kwargs):
        names = kwargs["features"]
        data = np.ones((2, len(names)), dtype=np.float32)
        data[1, :] = 3.0
        return CHUG_FEATURES.FeatureExtractionResult(
            feature_names=names,
            per_frame=data,
            n_frames=2,
        )

    written = CHUG_FEATURES.run(
        input_jsonl=chug_jsonl,
        output_jsonl=output,
        clips_dir=clips_dir,
        cache_dir=tmp_path / "cache",
        max_rows=None,
        runner=_FakeFeatureRunner(),
        extractor=fake_extract,
    )

    assert written == 1
    row = json.loads(output.read_text(encoding="utf-8"))
    assert row["feature_source"] == "chug-fr-ref-aligned"
    assert row["feature_alignment"] == "distorted_scaled_to_reference"
    assert row["feature_ref_src"] == "ref.mp4"
    assert row["feature_width"] == 32
    assert row["feature_height"] == 18
    assert row["split"] in {"train", "val", "test"}
    assert row["chug_split_key"] == "content-a.mp4"
    assert row["chug_split_policy"] == "content-name-blake2s-80-10-10"
    assert row["n_feature_frames"] == 2
    assert row["feature_ref_color_transfer"] == "smpte2084"
    assert row["feature_ref_transfer_class"] == "pq"
    assert row["feature_dis_color_transfer"] == "arib-std-b67"
    assert row["feature_dis_transfer_class"] == "hlg"
    assert row["feature_ref_color_primaries"] == "bt2020"
    assert row["feature_dis_color_space"] == "bt2020nc"
    assert row["feature_ref_max_content_nits"] == 1000.0
    assert row["feature_dis_max_average_nits"] == 400.0
    assert row["feature_ref_luma_std"] == 0.0
    assert row["feature_ref_sharpness_laplacian_var"] == 0.0
    assert row["feature_dis_luma_std"] > 0.0
    assert row["feature_dis_sharpness_laplacian_var"] > 0.0
    assert row["feature_delta_luma_std"] > 0.0
    assert row["feature_delta_noise_lap_mad"] > 0.0
    assert row["adm2"] == 2.0
    assert row["adm2_mean"] == 2.0


def test_chug_visual_signal_helper_reads_yuv10_luma(tmp_path: Path) -> None:
    yuv = tmp_path / "pattern.yuv"
    width, height = 8, 6
    luma = (np.indices((height, width)).sum(axis=0) % 2 * 1023).astype("<u2")
    chroma = np.full((height // 2, width // 2), 512, dtype="<u2")
    yuv.write_bytes(luma.tobytes() + chroma.tobytes() + chroma.tobytes())

    signals = CHUG_FEATURES.compute_visual_signals_from_yuv10(
        yuv,
        width=width,
        height=height,
    )

    assert signals["luma_std"] > 0.0
    assert signals["sharpness_laplacian_var"] > 0.0
    assert signals["highfreq_abs_mean"] > 0.0
    assert signals["noise_lap_mad"] > 0.0


def test_chug_hdr_metadata_helper_defaults_unknown_and_parses_side_data() -> None:
    metadata = CHUG_FEATURES.hdr_metadata_from_payload(
        {
            "streams": [
                {
                    "pix_fmt": "YUV420P10LE",
                    "codec_name": "HEVC",
                    "color_transfer": "SMPTE2084",
                    "color_primaries": "BT2020",
                    "side_data_list": [{"max_content": "1500", "max_average": "700"}],
                }
            ]
        }
    )

    assert metadata["pix_fmt"] == "yuv420p10le"
    assert metadata["codec_name"] == "hevc"
    assert metadata["transfer_class"] == "pq"
    assert metadata["color_space"] == "unknown"
    assert metadata["max_content_nits"] == 1500.0
    assert metadata["max_average_nits"] == 700.0

    missing = CHUG_FEATURES.hdr_metadata_from_payload(None)
    assert missing["transfer_class"] == "unknown"
    assert missing["color_primaries"] == "unknown"
    assert missing["max_content_nits"] is None


def test_chug_pairing_never_uses_identity_pairs_for_distorted_rows(
    tmp_path: Path,
) -> None:
    """Regression for ADR-0509: distorted rows must NEVER pair with themselves.

    The CHUG re-extract on 2026-05-18 (5992 rows, ~99 VMAF on every
    bitrate-ladder rung including 360p @ 0.2 Mbps) was caused by the
    operator routing through ``extract_k150k_features.py`` (FR-from-NR
    adapter: ref == distorted). This test pins the contract on the
    correct script (`chug_extract_features.py`): for every distorted
    row in a CHUG manifest with a matching reference, the emitted pair
    has ref_path != dis_path.
    """
    clips_dir = tmp_path / "clips"
    rows = [
        _chug_row(
            src="dist-360p.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=640,
            height=360,
            sha="0" * 64,
        ),
        _chug_row(
            src="dist-720p.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=1280,
            height=720,
            sha="1" * 64,
        ),
        _chug_row(
            src="dist-1080p.mp4",
            content="content-a.mp4",
            is_ref=False,
            width=1920,
            height=1080,
            sha="2" * 64,
        ),
        _chug_row(
            src="ref-1080p.mp4",
            content="content-a.mp4",
            is_ref=True,
            width=1920,
            height=1080,
            sha="3" * 64,
        ),
    ]

    pairs = CHUG_FEATURES.build_feature_pairs(rows, clips_dir=clips_dir)

    assert len(pairs) == 3
    for pair in pairs:
        assert (
            pair.ref_path != pair.dis_path
        ), f"identity pair detected (ADR-0509 regression): ref={pair.ref_path} dis={pair.dis_path}"
        assert pair.ref_row["chug_ref"] == 1
        assert pair.row["chug_ref"] == 0


def test_chug_pairing_skips_distorted_rows_without_matching_reference(
    tmp_path: Path,
) -> None:
    """Distorted rows whose ``chug_content_name`` has no reference are dropped.

    Prevents a silent fallback to identity-pairing in edge cases where a
    CHUG manifest is partially downloaded (some references missing).
    """
    clips_dir = tmp_path / "clips"
    rows = [
        _chug_row(
            src="orphan-dist.mp4",
            content="orphan-content.mp4",
            is_ref=False,
            width=640,
            height=360,
            sha="o" * 64,
        ),
    ]

    pairs = CHUG_FEATURES.build_feature_pairs(rows, clips_dir=clips_dir)

    assert pairs == []
