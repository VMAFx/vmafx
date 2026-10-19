<!-- markdownlint-disable MD060 -->
# `vmaf-tune tune-per-shot` — per-shot CRF tuning

`vmaf-tune tune-per-shot` splits a source into shots, finds the CRF that hits a
target VMAF for each shot, and writes an FFmpeg plan that encodes one segment
per shot and joins them with a concat-demuxer command. It does not run the
encodes itself. The design is in
[ADR-0392](../adr/0392-vmaf-tune-phase-d-per-shot.md); the tool overview is in
[`vmaf-tune.md`](vmaf-tune.md).

## How it works

1. **Detect shots.** The C-side [`vmaf-perShot`](vmaf-perShot.md) binary, which
   wraps TransNet V2 ([ADR-0223](../adr/0223-transnet-v2-shot-detector.md)),
   returns the shot boundaries. A uniform-time-window splitter then slices any
   shot longer than `--max-shot-duration`.
2. **Extract.** Each shot is cut to a temporary raw-YUV reference.
3. **Bisect.** The [target-VMAF bisect](vmaf-tune-bisect.md) runs per shot.
4. **Plan.** The tool emits one segment encode per shot and a final concat
   command.

The target-VMAF predicate stays pluggable: `--predicate-module
MODULE:CALLABLE` bypasses the default bisect, and the Python API accepts
`tune_per_shot(..., predicate=...)`.

## Quick start

A container source needs no pre-extraction. Its geometry, framerate and frame
count are auto-probed with `ffprobe` (ADR-0548):

```shell
vmaf-tune tune-per-shot \
    --src clip.mp4 \
    --target-vmaf 92 \
    --encoder libx264 \
    --output per_shot_encode.mp4 \
    --plan-out plan.json
```

A raw YUV source needs explicit geometry:

```shell
vmaf-tune tune-per-shot \
    --src ref.yuv \
    --width 1920 --height 1080 \
    --framerate 24 \
    --target-vmaf 92 \
    --encoder libx264 \
    --output per_shot_encode.mp4 \
    --plan-out plan.json
```

The plan goes to stdout as JSON unless `--plan-out` is set. Pass
`--script-out plan.sh` to also get a copy-paste shell script of the
per-segment and concat commands.

Exit code `2` means a setup failure: an unavailable score backend, missing
geometry for a raw source, or a failed bisect on a shot.

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--src PATH` | none (required) | Source video: raw YUV (`.yuv`, `.raw`) or any container (mp4, mkv, mov, ts, ...). For containers, `--width`, `--height`, `--framerate` and `--total-frames` come from ffprobe. |
| `--width`, `--height` | auto-probed | Source resolution. **Required for raw YUV.** Explicit values override the probe. |
| `--pix-fmt` | `yuv420p` | Pixel format, forwarded to `vmaf-perShot`. |
| `--framerate F` | auto-probed | Source framerate. Falls back to `24.0` when the probe cannot tell. |
| `--target-vmaf V` | `92.0` | Per-shot quality target. |
| `--encoder NAME` | `libx264` | Any registered codec adapter. |
| `--bitdepth N` | `8` | Source bit depth, forwarded to `vmaf-perShot`. One of `8`, `10`, `12`. |
| `--total-frames N` | `0` | Frame count for the [single-shot fallback](#single-shot-fallback). Auto-probed for containers. |
| `--scene-threshold X` | unset | Overrides `vmaf-perShot --diff-threshold`. Lower means more shots. Unset keeps the compiled default (`12.0` on 8-bit content). See [scene sensitivity](#tuning-scene-sensitivity). |
| `--max-shot-duration S` | `2.0` | Uniform-window splitter in seconds. A detected shot longer than `S` is cut into equal sub-shots. `0` disables it. |
| `--per-shot-bin PATH` | `vmaf-perShot` | Shot detector binary. |
| `--ffmpeg-bin PATH` | `ffmpeg` | FFmpeg binary. |
| `--vmaf-bin PATH` | `vmaf` | libvmaf CLI used by the per-shot scorer. |
| `--preset NAME` | adapter default | Preset forwarded to the bisect. |
| `--crf-min`, `--crf-max` | encoder absolute range | Inclusive CRF search bounds. Pass both or neither. |
| `--max-iterations N` | `8` | Encode and score iterations per shot. |
| `--vmaf-model NAME` | `vmaf_v1.0.16_3d0h` | VMAF model for the per-shot scorer. |
| `--neg` | off | Use the VMAF NEG model variant. |
| `--fast-nr` | off | NR early-elimination in each per-shot bisect, see [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md). |
| `--score-backend NAME` | `auto` | `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`. See [score backends](vmaf-tune-score-backend.md). |
| `--predicate-module SPEC` | none | `MODULE:CALLABLE` matching `(shot, target_vmaf, encoder) -> (crf, measured_vmaf)`. Bypasses the real bisect. |
| `--workdir PATH` | none | Parent of the per-run scratch directory. Overrides `VMAFTUNE_WORKDIR`. See [`vmaf-tune-compare.md`](vmaf-tune-compare.md#workdir-and-decode-concurrency). |
| `--max-concurrent-decodes N` | `1` | Cap on simultaneous reference-YUV decodes across the per-shot bisect threads. |
| `--output PATH` | `per_shot_encode.mp4` | Destination of the final concatenated encode. |
| `--segment-dir PATH` | see below | Directory for the per-shot segment files. |
| `--plan-out PATH` | stdout | Write the JSON plan here. |
| `--script-out PATH` | none | Also write a copy-paste shell script. |

### Segment directory

The directory for the concat listing (`concat.txt`) is, in order of priority:

1. `--segment-dir`;
2. `<plan-out parent>/segments`, when `--plan-out` is set;
3. `<output parent>/segments`.

When that directory is not writable (for example a read-only bind mount) the
tool prints a `WARN` on stderr and still exits `0`. The plan JSON stays the
authoritative deliverable
([ADR-0532](../adr/0532-per-shot-segments-readonly-cwd.md)).

## Plan JSON schema

```json
{
  "encoder": "libx264",
  "framerate": 24.0,
  "predicate": "bisect",
  "target_vmaf": 92.0,
  "shots": [
    {
      "start_frame": 0, "end_frame": 24,
      "crf": 22, "predicted_vmaf": 93.0,
      "bitrate_kbps": 5234.12
    },
    {
      "start_frame": 24, "end_frame": 72,
      "crf": 26, "predicted_vmaf": 92.5,
      "bitrate_kbps": 4182.44
    }
  ],
  "segment_commands": [
    ["ffmpeg", "-y", "-hide_banner", "-ss", "0.000000", "-i", "ref.mp4",
     "-frames:v", "24", "-c:v", "libx264", "-crf", "22",
     "/tmp/segments/shot_0000.mp4"]
  ],
  "concat_command": [
    "ffmpeg", "-y", "-hide_banner", "-f", "concat", "-safe", "0",
    "-i", "/tmp/segments/concat.txt", "-c", "copy", "out.mp4"
  ]
}
```

| Key | Meaning |
|---|---|
| `predicate` | `"bisect"` for the built-in search, otherwise the `--predicate-module` spec. |
| `start_frame`, `end_frame` | Half-open range: start inclusive, end exclusive (Python-slice convention). |
| `bitrate_kbps` | Encoded-segment bitrate measured by the bisect, $\dfrac{8 \cdot \mathrm{segment\_size\_bytes} / 1000}{\mathrm{shot\_duration\_s}}$ (ADR-0531). `null` with a custom `--predicate-module`, because no real encode runs. `vmaf-tune report` shows `null` as a dash in the per-shot table. |
| `segment_commands` | One FFmpeg command per shot. They honour the half-open range through `-frames:v`. |
| `concat_command` | The FFmpeg concat-demuxer command that joins the segments. |

The `vmaf-perShot` CSV and JSON sidecar uses an inclusive `end_frame`. The
planner normalises it to the half-open form.

## Single-shot fallback

If `vmaf-perShot` is not on `PATH`, or it exits non-zero, the planner falls
back to a single shot that covers the whole clip, `[0, --total-frames)`. That
keeps `tune-per-shot` usable as a smoke test on machines that have not built
the shot detector.

The uniform-window splitter still applies to the fallback. Even when the
detector returns one giant shot, `--max-shot-duration` (default `2.0` s)
slices it into about $\lceil \mathrm{duration} / \mathrm{window} \rceil$ equal
sub-shots, so the tuner
still produces a useful CRF timeline. `--max-shot-duration 0` restores the
historical single-shot behaviour
([ADR-0513](../adr/0513-per-shot-scene-threshold-and-1-shot-chart.md)).

## Tuning scene sensitivity

`vmaf-perShot` calls frame `N` a cut when the mean absolute luma delta against
frame `N-1` (in the 8-bit domain) crosses `--diff-threshold`. The compiled
default is `12.0`, tuned on the `testdata` fixtures
([ADR-0222](../adr/0222-vmaf-per-shot-tool.md)). Real content varies, and
`--scene-threshold` passes through to the C binary unchanged, so diagnostic
scripts that call `vmaf-perShot` directly use the same units.

Animated material with deep saturated transitions trips the heuristic
easily at the default. Other content behaves as follows:

| Content | Symptom at the default | Adjust |
|---|---|---|
| Short live action with low-contrast cuts (an underwater segment, indoor talking heads) | Cuts are missed | Lower the threshold to about `4` to `6`. |
| Motion-rich bursts inside one shot | Bursts are classed as fades and cut | Raise the threshold to about `18` to `25`. |

## What it does not do

!!! note "Limits"
    - It emits the plan only. Pipe `--script-out plan.sh` through `sh` to run
      it.
    - It does not use native per-codec mechanisms (x264 `--qpfile`, x265
      `--zones`, SVT-AV1 segment tables). Per-segment encoding plus the
      concat demuxer is the portable fallback.
    - It does not align GOPs to shot boundaries. Re-encoding each shot from
      frame 0 side-steps the problem.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.
- [`vmaf-perShot.md`](vmaf-perShot.md) — the shot detector binary.
- [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md) — the bisect run per shot.
- [`vmaf-tune-auto.md`](vmaf-tune-auto.md) — `auto` can score per shot in
  execute mode.
- [`vmaf-tune-fast-nr.md`](vmaf-tune-fast-nr.md) — `--fast-nr`.
- [ADR-0392](../adr/0392-vmaf-tune-phase-d-per-shot.md) — design rationale and
  decision matrix.
