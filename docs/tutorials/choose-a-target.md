<!-- markdownlint-disable MD013 MD046 MD060 -->
# Tutorial 3 — Choose a target

Ten minutes. At the end you have the CRF that keeps a chosen quality target
on this source, found by bisection with the scorer you already ran.

## What you need

Tutorial 2's two commands: one that encodes the reference at a given CRF and
one that scores the result. Choosing a target is looping them — no new
flags, no new tools.

## The loop

Pick the target first. Common picks:

- **95+** — visually transparent territory; expect large bitrates;
- **90** — a good streaming operating point for 1080p living-room content;
- **a relative target** — "within 2 points of the crf-18 encode" when you
  care about ratio, not absolutes.

Then bisect the CRF. The script below finds the CRF whose score stays at or
above the target with the smallest bitrate; run it with bash:

```bash
#!/usr/bin/env bash
# target.sh REF DIS_W H P — bisect CRF for a VMAF target (vmaf_v0.6.1, CPU)
set -euo pipefail
REF=$1; W=$2; H=$3; P=$4
TARGET=${TARGET:-90}
lo=14; hi=44
while [ "$lo" -lt "$hi" ]; do
    mid=$(( (lo + hi + 1) / 2 ))
    ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s "${W}x${H}" -r 24 \
        -i "$REF" -c:v libx264 -crf "$mid" -x264-params keyint=48 enc-t.mp4
    score=$(ffmpeg -loglevel error -i enc-t.mp4 -f rawvideo -pix_fmt yuv420p \
        -s "${W}x${H}" -r 24 -i testdata/ref_576x324_48f.yuv \
        -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null - 2>&1 \
        | grep -oP 'vmaf vmaf mean: \K[0-9.]+')
    echo "crf $mid -> $score"
    if python3 -c "exit(0 if float('$score') >= float('$TARGET') else 1)"; then
        lo=$mid
    else
        hi=$(( mid - 1 ))
    fi
done
echo "target CRF: $lo"
```

`bash target.sh testdata/ref_576x324_48f.yuv 576 324 420 TARGET=90` prints
one line per probe and ends with the CRF to use.

<!-- run-check:t3-encode exec=ffmpeg -->
```bash
ffmpeg -loglevel error -y -f rawvideo -pix_fmt yuv420p -s 576x324 -r 24 \
    -i testdata/ref_576x324_48f.yuv -c:v libx264 -crf 24 -x264-params keyint=48 \
    enc-target.mkv
```

<!-- run-check:t3-score exec=ffmpeg -->
```bash
ffmpeg -loglevel info -i enc-target.mkv -f rawvideo -pix_fmt yuv420p \
    -s 576x324 -r 24 -i testdata/ref_576x324_48f.yuv \
    -lavfi "[0:v][1:v]vmafx=model=version=vmaf_v0.6.1" -f null -
```

## Read the result

- The bisection is score-ordered, so it converges in about five probes —
  each probe is one encode plus one score, seconds on this fixture.
- The chosen CRF is only valid for **this source at this viewing condition**.
  A different resolution or a different model shifts every score; re-run the
  loop ([viewing assumptions](../reference/concepts.md#viewing-assumptions)).
- The fork's `vmaf-tune` tool automates this loop with corpus support and
  per-shot decisions when you outgrow the shell script
  ([vmaf-tune](../usage/vmaf-tune.md)).

## Try one thing

Change the target to 95 (`TARGET=95`) and rerun — the CRF drops and the
bitrate climbs. That trade is the whole point of
[bd-rate](../usage/bd-rate.md) measurement. Continue to
[Gate a build](gate-a-build.md).
