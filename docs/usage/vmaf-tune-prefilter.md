<!-- markdownlint-disable MD060 -->
# `vmaf-tune prefilter` — Pelorus deband and CRF joint autotune

`vmaf-tune prefilter` runs a VMAF-in-the-loop search that tunes the strengths
of the [Pelorus](https://github.com/VMAFx/pelorus) deband pre-filter and the
encoder CRF together. It returns the lowest-bitrate combination that reaches a
target VMAF, plus a ready-to-use ffmpeg `-vf` fragment. It is the control-plane
seam between vmafx and Pelorus
([ADR-1116](../adr/1116-autotune-prefilter-control-plane.md);
contract in Pelorus ADR-0110).

vmafx never runs the deband filter. It emits the ffmpeg
`-vf "pelorus_deband_vulkan=range=..:thry=..:.."` string and scores the encoded
output; the filter runs inside ffmpeg. The live loop therefore needs an ffmpeg
build that contains the Pelorus Vulkan deband filter. Without it the
subcommand refuses the live run with a clear message (exit code `2`) and
points you at `--smoke`.

## Install

The search engine is Optuna, which ships as the `[fast]` extra:

```shell
pip install 'vmaf-tune[fast]'
```

## Quick start

Smoke mode exercises the joint search end to end on a synthetic deband and CRF
surface. It needs no ffmpeg, no Vulkan and no GPU:

```shell
vmaf-tune prefilter --target-vmaf 92 --smoke --n-trials 40
```

Restrict the swept knobs with one or more `--sweep-knob` flags. The remaining
knobs stay at the filter default:

```shell
vmaf-tune prefilter --target-vmaf 93 --smoke \
  --sweep-knob range --sweep-knob grainy
```

## Live loop

The live loop needs a Pelorus-enabled ffmpeg, a source with explicit geometry
and a `vmaf` binary:

```shell
vmaf-tune prefilter \
  --src ref.yuv --width 1920 --height 1080 \
  --target-vmaf 93 --encoder libx264 \
  --crf-min 18 --crf-max 40 \
  --ffmpeg-bin /opt/ffmpeg-pelorus/bin/ffmpeg
```

The recommendation includes a `recommended_vf` fragment, for example:

```text
pelorus_deband_vulkan=range=12:thry=0.018:grainy=0.008
```

!!! warning "Live encode path is not hardware-verified"
    The live encode path is unit-tested with a mocked encode and score loop,
    but it has not been run against a real Pelorus-enabled ffmpeg. See
    `docs/state.md`, row `T-PREFILTER-LIVE-ENCODE-UNTESTED-2026-06-14`.

## Flags

`--target-vmaf` is always required. The live loop also needs `--src`, and
positive `--width` and `--height`.

| Flag | Default | Meaning |
|---|---|---|
| `--src PATH` | none | Source video. Required for the live loop. |
| `--width`, `--height` | `0` | Raw-YUV geometry. Required for the live loop. |
| `--pix-fmt` | `yuv420p` | ffmpeg pixel format for the probe encodes. |
| `--framerate` | `24.0` | Reference framerate. |
| `--duration` | `0.0` | Clip length in seconds, used to report kbps and to weight the bitrate term. At `0` the search optimises VMAF only and the reported bitrate is 0. |
| `--target-vmaf T` | none (required) | Quality target on the `[0, 100]` scale. |
| `--encoder` | `libx264` | Codec adapter that performs the post-deband encode. |
| `--preset` | `medium` | Encoder preset for the probe encodes. |
| `--filter` | `pelorus_deband` | Pre-encode filter adapter to autotune. `pelorus_deband` is the only choice. |
| `--sweep-knob KNOB` | all 10 | Repeatable. Restricts the swept deband knobs. |
| `--crf-min`, `--crf-max` | `18`, `40` | Joint TPE search range over CRF. |
| `--n-trials` | `60` (live), `40` (smoke) | TPE trial budget. |
| `--time-budget-s` | `600` | Soft wall-clock cap for the Optuna loop. |
| `--seed` | `0` | TPE sampler seed, for a reproducible search. |
| `--smoke` | off | Synthetic deband and CRF surface; no ffmpeg, Vulkan or GPU. |
| `--score-backend` | `auto` | Probe-score backend: `auto`, `cpu`, `cuda`, `sycl`, `hip` or `metal`. See [score backends](vmaf-tune-score-backend.md). |
| `--ffmpeg-bin`, `--vmaf-bin` | `ffmpeg`, `vmaf` | Tool paths. |
| `--vmaf-model` | `vmaf_v1.0.16_3d0h` | libvmaf model for the probe scores. |
| `--neg` | off | Use the VMAF NEG model variant. |
| `--encode-dir` | `.workingdir/cache/vmafx-tune/prefilter` | Scratch directory for probe encodes. |
| `--output` | stdout | JSON destination for the recommendation. |

The JSON result carries the recommended strengths (`recommended_deband`), the
fragment (`recommended_vf`), `recommended_crf`, `achieved_vmaf`,
`achieved_kbps` and a per-probe log (`probes`) with the VMAF of every trial.

## The frozen knob contract

The search space is exactly the 10 tunable knobs that Pelorus ADR-0110 freezes
(name, type, range, default). vmafx hard-codes this table in
`PELORUS_DEBAND_KNOBS`. Renaming, narrowing or retyping a knob is a
coordinated break of both repositories.

| Knob | Type | Range | Default | Meaning |
|---|---|---|---|---|
| `range` | int | 1 to 31 | 15 | Reference-sampling radius in pixels. |
| `thry` | float | 0.0 to 0.25 | 0.012 | Luma flat-test threshold. |
| `thrc` | float | 0.0 to 0.25 | 0.012 | Chroma flat-test threshold. |
| `grainy` | float | 0.0 to 0.4 | 0.006 | Luma grain amplitude. |
| `grainc` | float | 0.0 to 0.4 | 0.0 | Chroma grain amplitude. |
| `softness` | float | 0.0 to 1.0 | 0.5 | Soft-blend transition width. |
| `detail` | float | 0.0 to 0.25 | 0.06 | Detail-mask activity threshold. |
| `dither` | enum | 0 to 2 | 2 | 0 = none, 1 = bayer8, 2 = bluenoise. |
| `dynamic` | bool | 0 to 1 | 1 | Re-seed grain each frame. |
| `protect` | bool | 0 to 1 | 1 | Gate debanding off textured regions. |

The out-of-contract options `sample`, `blur`, `planes` and `meta` are never
swept. They are pipeline-topology or reporting switches that are set once per
run, outside the optimizer.

## How the joint search works

Each Optuna TPE trial proposes a full `(deband dict, crf)` pair. The loop then
runs `pelorus_deband_vulkan=...` into a hardware encode and a VMAF score for
that pair.

- **Objective.** `|achieved_vmaf - target| + lambda * kbps`, with a small
  lambda (`1e-4`) so the VMAF target dominates and ties break toward lower
  bitrate.
- **One study, eleven dimensions.** CRF is an ordinal integer dimension joined
  to the 10 deband dimensions. The two axes are co-optimised, not searched in
  nested loops, because debanding shifts the rate-quality curve and the axes
  are not separable.
- **Same engine as `fast`.** The `TPESampler` study is the one that
  [`vmaf-tune fast`](vmaf-tune-fast-path.md) uses (ADR-0276, ADR-0304).
  `prefilter` only builds the joint search space and the objective.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the tool overview.
- [`vmaf-tune-fast-path.md`](vmaf-tune-fast-path.md) — the sibling TPE search
  over CRF alone.
- [`vmaf-tune-score-backend.md`](vmaf-tune-score-backend.md) — scoring
  backends.
- [ADR-1116](../adr/1116-autotune-prefilter-control-plane.md) — design
  decision.
