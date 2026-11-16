<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorial 5 — Score live

Ten minutes. At the end you score a stream of encodes as they are produced —
the loop an encoding farm or a real-time monitor runs.

## What you need

Tutorials 1–4. "Live" here means: score each output as the encoder finishes
it, in the same shell, without stopping the pipeline.

## One score per encode, in a loop

Produce CRF 18…30 in steps and score each as it lands:

```bash
#!/usr/bin/env bash
# live.sh REF W H P — score every encode as it is produced
set -euo pipefail
REF=$1; W=$2; H=$3; P=$4
for crf in 18 22 26 30; do
    ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s "${W}x${H}" -r 24 \
        -i "$REF" -c:v libx264 -crf "$crf" -x264-params keyint=48 "live-$crf.mkv"
    score=$(ffmpeg -loglevel info -i "live-$crf.mkv" -f rawvideo -pix_fmt yuv420p \
        -s "${W}x${H}" -r 24 -i "$REF" \
        -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null - 2>&1 \
        | grep -oP 'vmafx \S+ mean: \K[0-9.]+')
    printf 'crf %s -> %s\n' "$crf" "$score"
done
```

```text
crf 18 -> <score>
crf 22 -> <score>
crf 26 -> <score>
crf 30 -> <score>
```

A descending table — the same verdict Tutorial 2 produced by hand, now
as a loop you can point at a real pipeline's output directory.

<!-- run-check:t5-encode exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -c:v libx264 -crf 22 -x264-params keyint=48 \
    encoded-live-22.mkv
```

<!-- run-check:t5-score exec=ffmpeg -->
```bash
ffmpeg -loglevel info -i encoded-live-22.mkv -f rawvideo -pix_fmt yuv420p \
    -s 576x324 -r 24 -i testdata/ref_576x324_48f.yuv \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null -
```

## Reading scores as they happen

- The loop is **sequential on purpose**: each score needs the whole clip, so
  "live" scores are per-finished-clip, not per-frame. Per-frame live scores
  exist in the API (`vmaf_score_pooled` per window, or the FFmpeg filter's
  windowed output) — see the
  [lifecycle docs](../api/lifecycle.md#vmafpoolingmethod).
- For farm-wide aggregation, keep the model, the geometry and the pooling
  fixed across every run, or the table compares apples and oranges
  ([what the score means](../reference/concepts.md)).
- The `vmaf-tune` tool and the
  [bd-rate tools](../usage/bd-rate.md) build on exactly this loop.

## Where to go next

- The [cookbook](../cookbook/index.md) — short recipes for 10-bit, offsets,
  subsampling and batch runs.
- [Gate a build](gate-a-build.md) for the CI shape of the same loop.
- The [chooser](../reference/chooser.md) when a tutorial's defaults do not
  match your content.
