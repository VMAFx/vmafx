# Research-0070: `vmaf-tune` codec-agnostic encode dispatcher

- **Status**: digest for [ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md)
- **Date**: 2026-05-03

## Problem

`tools/vmaf-tune/src/vmaftune/encode.py` was hard-coded to libx264:
the FFmpeg argv contained the literal `-c:v libx264 -preset $PRESET
-crf $CRF`, the version regex only knew x264's `core <N>` line, and
the corpus loop assumed CRF semantics throughout. Nine in-flight
codec adapter PRs (libx265, libsvtav1, libaom, libvvenc, NVENC, QSV,
AMF, VideoToolbox, plus the wave to 17) could not drive end-to-end
encodes until the harness stopped baking x264-shaped argv.

## Survey of codec quality knobs

| Codec | FFmpeg `-c:v` value(s) | Quality knob | Preset shape |
| --- | --- | --- | --- |
| libx264 | `libx264` | `-crf` (0..51) | `ultrafast`..`veryslow` |
| libx265 | `libx265` | `-crf` (0..51) | `ultrafast`..`veryslow` |
| libsvtav1 | `libsvtav1` | `-crf` (0..63) | `-preset 0..13` (integer) |
| libaom-av1 | `libaom-av1` | `-crf` + `-b:v 0` | `-cpu-used 0..8` |
| libvpx-vp9 | `libvpx-vp9` | `-crf` + `-b:v 0` + `-deadline good` | `-cpu-used 0..8` |
| libvvenc | `libvvenc` | `-qp` (constant-QP) or `-b:v` (target-rate) | `faster`/`fast`/`medium`/`slow`/`slower` |
| NVENC (h264/hevc/av1) | `h264_nvenc`, ... | `-cq` (with `-rc vbr`) | `p1`..`p7` |
| QSV (h264/hevc/av1/vp9) | `h264_qsv`, ... | `-global_quality` | `veryfast`..`veryslow` |
| AMF (h264/hevc/av1) | `h264_amf`, ... | `-qp_i` + `-qp_p` (with `-rc cqp`) | `-quality speed/balanced/quality` |
| VideoToolbox (h264/hevc) | `h264_videotoolbox` | `-q:v` (1..100, **higher = better**) | (none — VT picks internally) |

Two non-uniformities forced the dispatcher design:

1. **Quality knob is not always `-crf`.** NVENC uses `-cq`, VVenC
   uses `-qp`, QSV uses `-global_quality`, VideoToolbox uses `-q:v`
   on a 1..100 scale where higher is better.
2. **Preset slot is not always `-preset`.** libaom-av1 / libvpx-vp9
   take `-cpu-used`, AMF takes `-quality`. libsvtav1 keeps `-preset`
   but the value is an integer.

A single hard-coded ffmpeg invocation cannot serve all of these. The
adapter is the natural place to translate.

## Options

### A — Codec-agnostic dispatcher (chosen)

Adapter exposes `ffmpeg_codec_args(preset, quality) -> list[str]`
returning the codec-specific argv slice. Dispatcher concatenates it
into the otherwise-uniform `[ffmpeg, -y, ..., -i src, *codec_args,
*extra_params, output]` shape.

- **Pros**: One PR unblocks 17 adapters; harness stays
  codec-agnostic per ADR-0237 invariant; existing x264 argv is
  bit-preserved.
- **Cons**: Duck-typed contract; missing methods fall back silently.
  Mitigated by per-codec test cases pinning the expected argv shape.

### B — Per-codec driver functions

`run_encode_x264`, `run_encode_x265`, ... each in its own module.

- **Pros**: Strong isolation; type-checker can pin signatures per
  codec.
- **Cons**: Forks the harness loop 17 ways; rebase nightmare; every
  bisect / retry / version-parse fix must touch 17 files.
- **Verdict**: rejected — same anti-pattern ADR-0237 already ruled
  against.

### C — Strict `Protocol` with mandatory methods

Define a `runtime_checkable` `Protocol` requiring
`ffmpeg_codec_args`; reject adapters that don't implement it.

- **Pros**: Mis-typed adapters fail loudly at registration.
- **Cons**: Forces all 9 in-flight adapter PRs to land their
  `ffmpeg_codec_args` before the dispatcher merges — the whole point
  of this PR is to unblock them, so adding a synchronous-rendezvous
  requirement defeats the purpose.
- **Verdict**: rejected — task hard-rule explicitly demands the
  fallback path.

### D — Bump SCHEMA_VERSION to add a `quality` row column

Replace `crf` in the JSONL row with a codec-agnostic `quality`.

- **Pros**: Cleaner naming for non-CRF codecs.
- **Cons**: Forces every Phase B/C consumer to migrate; row schema
  contract (rebase-notes #0227) explicitly says "adding optional
  keys is fine; renaming requires bumping `SCHEMA_VERSION` *and*
  every downstream consumer in the same PR".
- **Verdict**: rejected — keep `crf` in the row, expose `quality`
  only as a request-side property.

## Reproducer

```shell
cd /home/kilian/dev/vmaf
pytest tools/vmaf-tune/tests/ -q
# 32 passed (13 existing + 19 new multi-codec dispatcher tests)
```

```shell
python -c "
from pathlib import Path
from vmaftune.encode import EncodeRequest, build_ffmpeg_command
req = EncodeRequest(
    source=Path('ref.yuv'), width=1920, height=1080, pix_fmt='yuv420p',
    framerate=24.0, encoder='libx264', preset='medium', crf=23,
    output=Path('out.mp4'),
)
print(build_ffmpeg_command(req))
"
```

Expected output contains `-c:v libx264 -preset medium -crf 23` —
unchanged from Phase A.

## References

- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — parent
  spec; codec-agnostic-search-loop invariant.
- [Research-0044](0044-quality-aware-encode-automation.md) —
  option-space digest for the parent ADR.
- FFmpeg encoder docs: `ffmpeg -h encoder=libx264`,
  `=libx265`, `=libsvtav1`, `=libaom-av1`, `=libvpx-vp9`,
  `=libvvenc`, `=h264_nvenc`, `=hevc_qsv`, `=h264_amf`,
  `=h264_videotoolbox`.
