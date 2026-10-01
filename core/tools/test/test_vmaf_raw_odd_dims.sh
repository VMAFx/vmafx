#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# ADR-1398: CLI accepts odd-sized raw YUV inputs for chroma-subsampled formats
# (4:2:0 and 4:2:2) using ceil chroma geometry, matching Y4M.
#
# Positive, negative and boundary tests:
# - Positive: 19x19 4:2:0, 1921x1081 4:2:0, 19x20 4:2:0, 20x19 4:2:0, 19x19 4:2:2
#   Scores for raw YUV must equal Y4M scores for the exact same sample content.
# - Boundary: 1x1 4:2:0 edge, 1x1 4:2:2 edge.
# - Negative: file too small and file size mismatch must fail cleanly (exit 2).
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_raw_odd_dims.scratch"
rm -rf "${WORK}"
mkdir -p "${WORK}"

python3 - "${WORK}" "${BIN}" <<'PY'
import json
import os
import subprocess
import sys
from pathlib import Path

work = Path(sys.argv[1])
bin_path = sys.argv[2]

def run_cmd(args):
    cmd = [bin_path] + args
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res

def ceil_div(a, b):
    return (a + b - 1) // b

def make_clip_data(w, h, pix_fmt, num_frames=2):
    # pix_fmt: '420' or '422'
    if pix_fmt == '420':
        cw = ceil_div(w, 2)
        ch = ceil_div(h, 2)
    elif pix_fmt == '422':
        cw = ceil_div(w, 2)
        ch = h
    else:
        raise ValueError(f"unsupported format {pix_fmt}")

    frame_bytes = w * h + 2 * cw * ch
    total_bytes = frame_bytes * num_frames

    # Deterministic pseudo-random bytes with visible differences
    ref = bytes((i * 17 + 11 + (i // frame_bytes) * 31) % 256 for i in range(total_bytes))
    dis = bytes((i * 23 + 47 + (i // frame_bytes) * 59) % 256 for i in range(total_bytes))
    return ref, dis, frame_bytes

def write_y4m(path, w, h, data, num_frames, pix_fmt='420'):
    chroma_tag = 'C420jpeg' if pix_fmt == '420' else 'C422'
    hdr = f"YUV4MPEG2 W{w} H{h} F25:1 Ip A1:1 {chroma_tag}\n".encode('ascii')
    frame_sz = len(data) // num_frames
    with open(path, 'wb') as f:
        f.write(hdr)
        for i in range(num_frames):
            f.write(b"FRAME\n")
            f.write(data[i * frame_sz : (i + 1) * frame_sz])

def test_raw_vs_y4m(name, w, h, pix_fmt='420', num_frames=2):
    ref_data, dis_data, frame_bytes = make_clip_data(w, h, pix_fmt, num_frames)

    ref_yuv = work / f"{name}_ref.yuv"
    dis_yuv = work / f"{name}_dis.yuv"
    ref_yuv.write_bytes(ref_data)
    dis_yuv.write_bytes(dis_data)

    out_yuv = work / f"{name}_yuv.json"
    res_yuv = run_cmd([
        '-r', str(ref_yuv), '-d', str(dis_yuv),
        '-w', str(w), '-h', str(h), '-p', pix_fmt, '-b', '8',
        '--no_prediction', '--feature', 'psnr',
        '--json', '-o', str(out_yuv)
    ])
    assert res_yuv.returncode == 0, f"YUV failed for {name}: {res_yuv.stderr}"
    assert out_yuv.exists(), f"Output JSON missing for {name} YUV"

    # Only 420 is tested against Y4M directly because Y4M C422 reader resamples
    # chroma to jpeg siting, while raw does not.
    if pix_fmt == '420':
        ref_y4m = work / f"{name}_ref.y4m"
        dis_y4m = work / f"{name}_dis.y4m"
        write_y4m(ref_y4m, w, h, ref_data, num_frames, pix_fmt)
        write_y4m(dis_y4m, w, h, dis_data, num_frames, pix_fmt)

        out_y4m = work / f"{name}_y4m.json"
        res_y4m = run_cmd([
            '-r', str(ref_y4m), '-d', str(dis_y4m),
            '--no_prediction', '--feature', 'psnr',
            '--json', '-o', str(out_y4m)
        ])
        assert res_y4m.returncode == 0, f"Y4M failed for {name}: {res_y4m.stderr}"
        assert out_y4m.exists(), f"Output JSON missing for {name} Y4M"

        with open(out_yuv) as f:
            yuv_json = json.load(f)
        with open(out_y4m) as f:
            y4m_json = json.load(f)

        for m in ('psnr_y', 'psnr_cb', 'psnr_cr'):
            yuv_score = yuv_json['pooled_metrics'][m]['mean']
            y4m_score = y4m_json['pooled_metrics'][m]['mean']
            diff = abs(yuv_score - y4m_score)
            assert diff < 1e-5, f"{name} {m} score mismatch: yuv={yuv_score}, y4m={y4m_score}"
        print(f"ok: {name} (w={w}, h={h}, {pix_fmt}) bit-exact YUV == Y4M")
    else:
        with open(out_yuv) as f:
            yuv_json = json.load(f)
        for m in ('psnr_y', 'psnr_cb', 'psnr_cr'):
            assert m in yuv_json['pooled_metrics'], f"{name} missing metric {m}"
        print(f"ok: {name} (w={w}, h={h}, {pix_fmt}) raw YUV success")

print("--- Positive and Boundary tests ---")
# 1. 19x19 4:2:0 odd width and odd height
test_raw_vs_y4m("odd_19x19_420", 19, 19, '420', num_frames=3)

# 2. 1921x1081 4:2:0 odd width and odd height (HD+1)
test_raw_vs_y4m("odd_1921x1081_420", 1921, 1081, '420', num_frames=2)

# 3. 19x20 4:2:0 odd width, even height
test_raw_vs_y4m("odd_19x20_420", 19, 20, '420', num_frames=2)

# 4. 20x19 4:2:0 even width, odd height
test_raw_vs_y4m("odd_20x19_420", 20, 19, '420', num_frames=2)

# 5. 19x19 4:2:2 odd width
test_raw_vs_y4m("odd_19x19_422", 19, 19, '422', num_frames=2)

# 6. 1x1 boundary edge 4:2:0
test_raw_vs_y4m("boundary_1x1_420", 1, 1, '420', num_frames=1)

# 7. 1x1 boundary edge 4:2:2
test_raw_vs_y4m("boundary_1x1_422", 1, 1, '422', num_frames=1)

print("\n--- Negative tests ---")
# 8. File too small: 19x19 4:2:0 frame is 561 bytes, file has only 560 bytes
too_small = work / "too_small.yuv"
too_small.write_bytes(b"\x00" * 560)
res = run_cmd([
    '-r', str(too_small), '-d', str(too_small),
    '-w', '19', '-h', '19', '-p', '420', '-b', '8',
    '--no_prediction', '--feature', 'psnr'
])
assert res.returncode == 2, f"expected exit code 2, got {res.returncode}"
assert "file too small for declared geometry" in res.stderr
print("ok: file too small exits 2 with clear diagnostic")

# 9. File size mismatch: 19x19 4:2:0 frame is 561 bytes, file has 661 bytes (1.18 frames)
mismatch = work / "mismatch.yuv"
mismatch.write_bytes(b"\x00" * 661)
res = run_cmd([
    '-r', str(mismatch), '-d', str(mismatch),
    '-w', '19', '-h', '19', '-p', '420', '-b', '8',
    '--no_prediction', '--feature', 'psnr'
])
assert res.returncode == 2, f"expected exit code 2, got {res.returncode}"
assert "file size mismatch" in res.stderr
print("ok: file size mismatch exits 2 with clear diagnostic")

# 10. Non-positive dimensions: --width 0 / --height 0
res_w0 = run_cmd([
    '-r', str(too_small), '-d', str(too_small),
    '-w', '0', '-h', '19', '-p', '420', '-b', '8',
    '--no_prediction', '--feature', 'psnr'
])
assert res_w0.returncode != 0
assert "--width must be > 0" in res_w0.stderr
print("ok: zero width rejected by CLI parser")

res_h0 = run_cmd([
    '-r', str(too_small), '-d', str(too_small),
    '-w', '19', '-h', '0', '-p', '420', '-b', '8',
    '--no_prediction', '--feature', 'psnr'
])
assert res_h0.returncode != 0
assert "--height must be > 0" in res_h0.stderr
print("ok: zero height rejected by CLI parser")

PY

rm -rf "${WORK}"
echo "PASS: all positive, negative and boundary tests for raw odd dimensions succeeded"
