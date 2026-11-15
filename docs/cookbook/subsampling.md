<!-- markdownlint-disable MD013 MD046 MD060 -->
# Recipe — Score non-420 subsampling

**Situation:** your inputs are 4:2:2 or 4:4:4 — pro-video masters, screen
captures, chroma-heavy material.

The `-p` / `--pixel_format` flag takes `420`, `422` or `444`, and must state
what the file actually is. Both inputs must share the format — the engine
compares sample grids, and a 420 file read as 444 is a corrupt geometry, not
a quality statement.

## Convert the fixture pair to 4:2:2

FFmpeg upsamples chroma losslessly from 4:2:0 to 4:2:2/4:4:4 (luma untouched,
chroma duplicated), so the converted pair scores the same as the original:

<!-- run-check:subs-convert exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -pix_fmt yuv422p -f rawvideo \
    ref422_576x324_48f.yuv
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv -pix_fmt yuv422p -f rawvideo \
    dis422_576x324_48f.yuv
```

## Score the 4:2:2 pair

<!-- run-check:subs-score exec=cli -->
```bash
vmaf -r ref422_576x324_48f.yuv -d dis422_576x324_48f.yuv \
    -w 576 -h 324 -p 422 -b 8 -m version=vmaf_v0.6.1 --backend cpu \
    --json -o score422.json
```

## Reading it

- The upsampled pair's pooled mean matches the 420 run (94.32301) — chroma
  upsampling does not invent luma damage. If it drifts, the geometry or the
  `-p` value is wrong.
- For real chroma damage, luma-only metrics are blind by design; add
  `ciede2000` or `delta_e_itp` to the feature list
  ([chroma subsampling](../reference/glossary.md#chroma-subsampling),
  [CIEDE2000](../metrics/ciede.md)):
  `--feature ciede` on the same command.
- HDR colour work uses `delta_e_itp` with explicit transfer and range
  options ([ΔE-ITP](../metrics/delta_e_itp.md)).

See the [glossary](../reference/glossary.md) — terms used here:
[chroma subsampling](../reference/glossary.md#chroma-subsampling).
