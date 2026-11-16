<!-- markdownlint-disable MD013 MD046 MD060 -->
# Recipe — Score with a frame offset

**Situation:** the distorted stream starts a few frames late (or early) — a
truncated encoder warm-up, a missing first GOP. Scoring straight away pairs
every frame with the wrong reference and the number drops for no reason.

## The flags

`--frame_skip_ref N` and `--frame_skip_dist N` skip the first N frames of
each input; `--frame_cnt N` caps how many frames are scored after the skip.
Skip exactly the offset on the late stream and cap both to the common
length.

## Measure the offset first

Score with no skip, then with candidate skips, and watch for the plateau:

<!-- run-check:offset-zero exec=cli -->
```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 --backend cpu \
    --frame_cnt 40 --json -o offset0.json
```

<!-- run-check:offset-four exec=cli -->
```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 --backend cpu \
    --frame_skip_dist 4 --frame_cnt 40 --json -o offset4.json
```

## Reading it

```bash
python3 -c "import json; \
print('skip 0:', json.load(open('offset0.json'))['pooled_metrics']['vmaf']['mean']); \
print('skip 4:', json.load(open('offset4.json'))['pooled_metrics']['vmaf']['mean'])"
```

- The fixture pair is aligned, so both runs report the same frames' content
  and the means agree to the frame-content difference — if your offset is
  real, the correctly-skipped run scores **higher**, and the score stops
  climbing once the offset is fully skipped. That plateau is the offset.
- On the fixture pair (no offset) the skip run scores a different 40-frame
  window, so small differences are expected; the recipe's point is the
  method, not a pinned digit.
- In FFmpeg, alignment is timestamps, not frame counts: `setpts=PTS-STARTPTS`
  and `trim` do this job inside a filter graph
  ([frame alignment](../reference/glossary.md#frame-alignment),
  [score exactly N frames](../usage/ffmpeg.md#score-exactly-n-frames)).

See the [glossary](../reference/glossary.md) — terms used here:
[frame alignment](../reference/glossary.md#frame-alignment).
