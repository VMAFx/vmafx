<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorial 4 — Gate a build

Ten minutes. At the end you have a small pass/fail script that refuses an
encode whose score drops below a floor — the shape every quality gate in CI
takes.

## What you need

Tutorials 1 and 2. The gate is three steps: score, compare against a floor,
exit non-zero on failure.

## The gate script

```bash
#!/usr/bin/env bash
# gate.sh DISTORTED REF W H P FLOOR — exit 1 when the score is under FLOOR
set -euo pipefail
DIS=$1; REF=$2; W=$3; H=$4; P=$5; FLOOR=$6
ffmpeg -loglevel error -i "$DIS" -f rawvideo -pix_fmt yuv420p \
    -s "${W}x${H}" -r 24 -i "$REF" \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1:log_fmt=json:log_path=gate.json" \
    -f null -
score=$(python3 -c "import json; print(json.load(open('gate.json'))['pooled_metrics']['vmaf']['mean'])")
echo "pooled vmaf: $score (floor $FLOOR)"
python3 -c "exit(0 if float('$score') >= float('$FLOOR') else 1)" \
    || { echo "QUALITY GATE FAILED"; exit 1; }
echo "QUALITY GATE PASSED"
```

`bash gate.sh encoded-hi.mkv testdata/ref_576x324_48f.yuv 576 324 420 90`
prints the score and the verdict. Tutorial 2's `encoded-hi.mkv` passes a 90
floor; `encoded-lo.mkv` is the interesting case — lower the floor or expect
the gate to refuse it.

## Gate the fixture pair itself

The fixture pair is the reference for itself minus distortion, so its score
is the sanity floor for the whole setup. If this run does not reproduce the
snapshot, every number from this machine is suspect:

<!-- run-check:t4-fixture exec=cli -->
```bash
vmaf -r testdata/ref_576x324_48f.yuv -d testdata/dis_576x324_48f.yuv \
    -w 576 -h 324 -p 420 -b 8 -m version=vmaf_v0.6.1 --backend cpu \
    --json -o t4-score.json
```

```text
94.32301
```

That is the CPU golden snapshot's pooled mean
(`testdata/scores_cpu_576.json`) — if you see it, your toolchain scores
exactly like the recorded run.

## Read the gate

- Gate on the **pooled mean** for overall quality; gate on `min` (or
  `perc5`) when the worst moment matters
  ([pooling](../reference/glossary.md#pooling)).
- The floor is a product decision, not a metric constant — 90 on one source
  is not 90 on another ([chooser](../reference/chooser.md)).
- Score the **decoded output**, never the bitstream
  ([FAQ: bitstreams](../reference/faq.md#q-can-i-pass-encoded-h264-vp9-hevc-bitstreams-to-vmaf-as-input-upstream-netflixvmaf55)).

## Try one thing

Run the gate on `encoded-lo.mkv` with the same floor and watch it fail with
exit 1 — then wire it into CI as a step. Continue to
[Score live](score-live.md).
