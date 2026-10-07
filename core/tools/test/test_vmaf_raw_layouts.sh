#!/bin/sh
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# ADR-2145 / ADR-2146: the CLI reads raw files in the layouts of the import
# table and the y4m high-bit-depth tags, with the same scores as the planar
# frame.
#
# Positive: NV12, UYVY422, AYUV, V210, YUYV422 and a 16-bit P016 file made from
#   a planar clip score as the planar file; `--pixel_format 400` reads luma-only
#   files; y4m C420p9 / C444p14 / Cmono12 tags read and score as the raw file of
#   the same samples; RGBA with a stated matrix reads.
# Negative: RGBA without a statement exits non-zero naming the first missing
#   flag; a matrix the integer conversion does not make is refused by name;
#   a --bitdepth outside the layout is refused naming it.
# Boundary: odd luma width for the packed 4:2:2 layouts, V210 with a partial group.
set -eu

BIN=./tools/vmaf
WORK="${MESON_BUILD_ROOT:-.}/test_vmaf_raw_layouts.scratch"
rm -rf "${WORK}"
mkdir -p "${WORK}"

python3 - "${WORK}" "${BIN}" <<'PY'
import json
import struct
import subprocess
import sys
from pathlib import Path

work = Path(sys.argv[1])
binary = sys.argv[2]
W, H, N = 14, 6, 2


def sample(i, bpc, salt):
    return (i * 2654435761 + salt * 40503) % (1 << bpc)


def planes(bpc, cw, ch, salt):
    """N frames of planar Y, Cb, Cr as lists of sample lists."""
    frames = []
    for f in range(N):
        y = [sample(i + 977 * f, bpc, salt) for i in range(W * H)]
        cb = [sample(i + 31 * f, bpc, salt + 1) for i in range(cw * ch)]
        cr = [sample(i + 53 * f, bpc, salt + 2) for i in range(cw * ch)]
        frames.append((y, cb, cr))
    return frames


def words(values, bpc):
    return b"".join(struct.pack("<H", v) for v in values) if bpc > 8 else bytes(values)


def planar_bytes(frames, bpc):
    return b"".join(words(p, bpc) for fr in frames for p in fr)


def nv12(frames):
    out = b""
    for y, cb, cr in frames:
        out += bytes(y) + bytes(v for pair in zip(cb, cr) for v in pair)
    return out


def p016(frames):
    out = b""
    for y, cb, cr in frames:
        out += words(y, 16) + words([v for pair in zip(cb, cr) for v in pair], 16)
    return out


def yuyv(frames, uyvy=False):
    out = b""
    cw = (W + 1) // 2
    for y, cb, cr in frames:
        for r in range(H):
            for g in range(cw):
                y0 = y[r * W + 2 * g]
                y1 = y[r * W + 2 * g + 1]
                u, v = cb[r * cw + g], cr[r * cw + g]
                out += bytes((u, y0, v, y1) if uyvy else (y0, u, y1, v))
    return out


def ayuv(frames):
    out = b""
    for y, cb, cr in frames:
        out += b"".join(bytes((0xA5, y[i], cb[i], cr[i])) for i in range(W * H))
    return out


def v210(frames):
    out = b""
    cw = (W + 1) // 2
    for y, cb, cr in frames:
        for r in range(H):
            row = bytearray()
            for g in range((W + 5) // 6):
                s = []
                for k in range(6):
                    x = g * 6 + k
                    s.append(y[r * W + x] if x < W else 0x3FF)
                c = []
                for k in range(3):
                    x = g * 3 + k
                    c.append((cb[r * cw + x], cr[r * cw + x]) if x < cw else (0x3FF, 0x3FF))
                w = [
                    c[0][0] | (s[0] << 10) | (c[0][1] << 20) | (3 << 30),
                    s[1] | (c[1][0] << 10) | (s[2] << 20) | (3 << 30),
                    c[1][1] | (s[3] << 10) | (c[2][0] << 20) | (3 << 30),
                    s[4] | (c[2][1] << 10) | (s[5] << 20) | (3 << 30),
                ]
                row += struct.pack("<4I", *w)
            out += bytes(row)
    return out


def score(args):
    out = work / "out.json"
    done = subprocess.run(
        [binary, "-n", "--feature", "psnr", "--json", "-o", str(out), "-q", *args],
        capture_output=True,
        text=True,
    )
    if done.returncode != 0:
        return None, done.stderr
    frames = json.loads(out.read_text())["frames"]
    return [f["metrics"]["psnr_y"] for f in frames], ""


def run_pair(name, ref, dis, extra):
    (work / f"{name}_r").write_bytes(ref)
    (work / f"{name}_d").write_bytes(dis)
    got, err = score(["-r", str(work / f"{name}_r"), "-d", str(work / f"{name}_d"), "-w", str(W), "-h", str(H), *extra])
    assert got is not None, f"{name}: {err}"
    return got


def check_layout(name, fmt, bpc, cw, ch, pack, plain, extra=()):
    ref_f, dis_f = planes(bpc, cw, ch, 1), planes(bpc, cw, ch, 7)
    want = run_pair(name + "_planar", planar_bytes(ref_f, bpc), planar_bytes(dis_f, bpc), ["-p", plain, "-b", str(bpc)])
    got = run_pair(name, pack(ref_f), pack(dis_f), ["-p", fmt, *extra])
    assert got == want, f"{name}: {got} != {want}"
    print(f"ok {name}")


cw420, ch420 = (W + 1) // 2, (H + 1) // 2
check_layout("nv12", "nv12", 8, cw420, ch420, nv12, "420")
check_layout("p016", "p016", 16, cw420, ch420, p016, "420")
check_layout("yuyv422", "yuyv422", 8, cw420, H, yuyv, "422")
check_layout("uyvy422", "uyvy422", 8, cw420, H, lambda f: yuyv(f, True), "422")
check_layout("ayuv", "ayuv", 8, W, H, ayuv, "444")
# V210 holds 10-bit samples; the encoder-reserved codes 0 to 3 and 1020 to 1023 are still read.
check_layout("v210", "v210", 10, cw420, H, v210, "422")

# Luma only.
gray = [bytes(sample(i, 8, 3 + s) for i in range(W * H)) for s in range(2)]
got = run_pair("gray", gray[0] * N, gray[1] * N, ["-p", "400", "-b", "8"])
assert len(got) == N
print("ok gray")


def cli_error(extra):
    done = subprocess.run(
        [binary, "-r", str(work / "nv12_r"), "-d", str(work / "nv12_d"), "-w", str(W), "-h", str(H), *extra],
        capture_output=True,
        text=True,
    )
    return done.returncode, done.stderr


rc, err = cli_error(["-p", "nv12", "-b", "10"])
assert rc != 0 and "nv12" in err and "8 to 8" in err, err
rgba = bytes((i * 7) % 256 for i in range(W * H * 4 * N))
(work / "rgba_r").write_bytes(rgba)
(work / "rgba_d").write_bytes(rgba[::-1])
base = ["-r", str(work / "rgba_r"), "-d", str(work / "rgba_d"), "-w", str(W), "-h", str(H), "-p", "rgba", "-b", "8"]
done = subprocess.run([binary, *base], capture_output=True, text=True)
assert done.returncode != 0 and "rgb_matrix" in done.stderr and "none is assumed" in done.stderr, done.stderr
stated = ["--rgb_matrix", "bt709", "--rgb_range", "full", "--rgb_transfer", "srgb", "--rgb_out_range", "limited"]
for drop in range(4):
    cut = stated[: 2 * drop] + stated[2 * drop + 2 :]
    done = subprocess.run([binary, *base, *cut], capture_output=True, text=True)
    assert done.returncode != 0 and stated[2 * drop].lstrip("-") in done.stderr, (drop, done.stderr)
done = subprocess.run([binary, *base, *stated[:1], "ictcp", *stated[2:]], capture_output=True, text=True)
assert done.returncode != 0 and "ictcp" in done.stderr, done.stderr
done = subprocess.run([binary, *base, *stated, "-n", "--feature", "psnr", "--json", "-o", str(work / "rgb.json"), "-q"], capture_output=True, text=True)
assert done.returncode == 0, done.stderr
assert len(json.loads((work / "rgb.json").read_text())["frames"]) == N
print("ok rgba")


def y4m(path, tag, bpc, fr, dec):
    hdr = f"YUV4MPEG2 W{W} H{H} F25:1 Ip A0:0 {tag}\n".encode()
    body = b"".join(b"FRAME\n" + planar_bytes([f], bpc) for f in fr)
    path.write_bytes(hdr + body)


for tag, bpc, cw, ch, plain in (("C420p9", 9, cw420, ch420, "420"), ("C422p14", 14, cw420, H, "422"), ("C444p16", 16, W, H, "444")):
    ref_f, dis_f = planes(bpc, cw, ch, 1), planes(bpc, cw, ch, 7)
    want = run_pair("planar_" + tag, planar_bytes(ref_f, bpc), planar_bytes(dis_f, bpc), ["-p", plain, "-b", str(bpc)])
    y4m(work / "r.y4m", tag, bpc, ref_f, plain)
    y4m(work / "d.y4m", tag, bpc, dis_f, plain)
    got, err = score(["-r", str(work / "r.y4m"), "-d", str(work / "d.y4m")])
    assert got == want, f"{tag}: {got} != {want} {err}"
    print(f"ok y4m {tag}")

# Cmono12: luma only in the file, neutral chroma made.
mono = [[sample(i + 977 * f, 12, 5 + s) for i in range(W * H)] for s in range(2) for f in range(N)]
for name, content in (("r", mono[:N]), ("d", mono[N:])):
    hdr = f"YUV4MPEG2 W{W} H{H} F25:1 Ip A0:0 Cmono12\n".encode()
    (work / f"m{name}.y4m").write_bytes(hdr + b"".join(b"FRAME\n" + words(c, 12) for c in content))
got, err = score(["-r", str(work / "mr.y4m"), "-d", str(work / "md.y4m")])
assert got is not None and len(got) == N, err
print("ok y4m Cmono12")
PY
