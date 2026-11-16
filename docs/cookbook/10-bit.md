<!-- markdownlint-disable MD013 MD046 MD060 -->
# Recipe — Score a 10-bit pair

**Situation:** your inputs are 10-bit YUV (or you want the scoring done at
10-bit, which the v1 models prefer for banding accuracy).

The flag pair is `--pixel_format 420 --bitdepth 10` (short forms `-p 420 -b
10`); the `-b` value is what changes against the 8-bit tutorial. The engine
reads each sample as 10 little-endian bits, so the file must really be
10-bit — scoring 8-bit bytes as 10-bit halves the frame and corrupts every
number.

## Convert the fixture pair to 10-bit

FFmpeg widens 8-bit YUV to 10-bit losslessly (values shift into the 10-bit
range):

<!-- run-check:10bit-convert-ref exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -pix_fmt yuv420p10le -f rawvideo \
    ref10_576x324_48f.yuv
```

<!-- run-check:10bit-convert-dis exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/dis_576x324_48f.yuv -pix_fmt yuv420p10le -f rawvideo \
    dis10_576x324_48f.yuv
```

## Score the 10-bit pair

<!-- run-check:10bit-score exec=cli -->
```bash
vmaf -r ref10_576x324_48f.yuv -d dis10_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 10 -m version=vmaf_v0.6.1 --backend cpu \
    --json -o score10.json
```

## Reading it

- The 10-bit score of the widened pair matches the 8-bit run — widening is
  lossless, so the pooled mean stays 94.32301 (the
  [CPU snapshot](../reference/concepts.md) value). If it does not, the
  conversion or the geometry is off, not the model.
- Real 10-bit content (not widened 8-bit) scores differently from its 8-bit
  round-trip when banding is involved — that difference is what 10-bit
  scoring is for ([models: v1](../models/v1.md)).
- CAMBI always measures at 10-bit internally and converts other depths
  itself ([CAMBI](../metrics/cambi.md)).

See the [glossary](../reference/glossary.md) — terms used here:
[10-bit](../reference/glossary.md#10-bit).
