<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorial 2 — Compare two encodes

Ten minutes. At the end you have two scores for the same reference encoded
two ways, and a one-line rule for reading the comparison.

## What you need

Tutorial 1's setup, plus FFmpeg with `libx264` (any distribution build has
it) to produce the two encodes. The reference is raw YUV, so the encode
commands carry its geometry too.

## Encode the reference twice

Two constant-rate encodes of the same source, one lighter on bitrate than
the other:

<!-- run-check:t2-encode-hi exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -c:v libx264 -crf 18 -x264-params keyint=48 \
    encoded-hi.mkv
```

<!-- run-check:t2-encode-lo exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -c:v libx264 -crf 34 -x264-params keyint=48 \
    encoded-lo.mkv
```

## Score both against the reference

The encodes are `.mkv` containers — FFmpeg decodes them, so the score run
happens on the decoded frames. Use the `vmafx` filter in a filter graph:
**pad 0 is the distorted (encoded) stream, pad 1 the reference**
([input ordering](../usage/ffmpeg.md#input-ordering)).

<!-- run-check:t2-score-hi exec=ffmpeg -->
```bash
ffmpeg -loglevel info -i encoded-hi.mkv -f rawvideo -pix_fmt yuv420p \
    -s 576x324 -r 24 -i testdata/ref_576x324_48f.yuv \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null -
```

<!-- run-check:t2-score-lo exec=ffmpeg -->
```bash
ffmpeg -loglevel info -i encoded-lo.mkv -f rawvideo -pix_fmt yuv420p \
    -s 576x324 -r 24 -i testdata/ref_576x324_48f.yuv \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null -
```

Two lines come out, one per encode:

```text
vmafx vmaf mean: <higher score for encoded-hi>
vmafx vmaf mean: <lower score for encoded-lo>
```

The crf-18 encode scores higher than the crf-34 encode — that ordering is
the whole verdict. The absolute values depend on your libx264 build, so this
tutorial pins the **ordering**, not the digits.

## Read the comparison

- Higher VMAF = closer to the reference **at the model's viewing condition**
  — not "objectively better" ([what the score means](../reference/concepts.md)).
- Both runs share the model (`vmaf_v0.6.1`), so the comparison is valid;
  comparing a `v0.6.1` score against a default-model score is not
  ([model mismatch](../reference/concepts.md#model-mismatch)).
- For encoder pairs where fake gains are a risk (sharpening), repeat the run
  with the NEG model
  ([NEG](../reference/glossary.md#neg), [chooser](../reference/chooser.md)).

## Try one thing

Swap the pads in the last command (`[1:v][0:v]`) and rerun — the filter
warns about the direction and the mean changes. That is the wrong-direction
score from the [concept guide](../reference/concepts.md#reversed-inputs),
observed live. Continue to [Choose a target](choose-a-target.md).
