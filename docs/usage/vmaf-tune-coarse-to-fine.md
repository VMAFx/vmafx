<!-- markdownlint-disable MD013 MD060 -->
# `vmaf-tune --coarse-to-fine` — two-pass CRF search

`--coarse-to-fine` finds the CRFs worth measuring for a target VMAF in about
15 encodes instead of the 52 a full 0..51 sweep needs, with no measurable
quality regression. `vmaf-tune recommend` always uses it; `vmaf-tune corpus`
uses it when the flag is given.

Use it when the only question is "what is the lowest-bitrate CRF whose VMAF still
meets my target?" and you do not need every CRF in the corpus.

## Quick start

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --encoder libx264 \
    --preset medium \
    --coarse-to-fine --target-vmaf 92 \
    --output corpus_c2f.jsonl
```

`--crf` is not needed: the search generates the CRF axis. `--target-vmaf` is
optional for `corpus`; without it both passes still run and the fine pass
refines around the highest-VMAF coarse point. `--preset` is required.

## How it works

1. **Coarse pass.** Encode and score CRFs `--coarse-step` apart across the
   encoder's search window: for libx264 the window is 10..50, so the grid is
   `10, 20, 30, 40, 50` (5 encodes). See [Search window](#search-window).
2. **Pick the centre.** With a target, the centre is the highest coarse CRF
   whose VMAF meets the target (the lowest `-q:v` for VideoToolbox, whose
   value rises with quality). When no coarse CRF meets it, or when no target
   was given, the centre is the coarse CRF with the highest VMAF.
3. **Fine pass.** Encode and score every CRF within `--fine-radius` of the
   centre at `--fine-step` spacing, skipping CRFs the coarse pass already
   measured. With defaults that is the 10 unique CRFs around the centre, for
   example `25..29` and `31..35` when the centre is `30`.
4. **Emit.** Write the union of visited rows to the normal corpus JSONL schema.

The search runs once per `--preset`.

### One-pass shortcut

When the highest coarse CRF already meets the target, the search stops after
the coarse pass. Lower bitrate would need CRFs above the coarse grid, which the
fine pass does not probe anyway.

## Flags

The same flags exist on `corpus` and `recommend`.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--coarse-to-fine` | off | Enable the search on `corpus`. `recommend` runs it unconditionally, so the flag is accepted there but redundant. |
| `--target-vmaf V` | none | Target that centres the fine pass. Optional for `corpus`, required for `recommend`. |
| `--coarse-step N` | `10` | CRF spacing of the coarse pass across the encoder's window; for libx264 the grid is `[10, 20, 30, 40, 50]`. |
| `--fine-radius R` | `5` | Fine pass covers the centre CRF plus or minus `R`. |
| `--fine-step S` | `1` | CRF spacing inside the fine window. |

### Search window

The coarse grid and the fine pass stay inside a window that comes from the
encoder: the libx264-shaped window 10..50 intersected with the adapter's
`quality_range` (`corpus.coarse_search_window`). No CLI flag changes it; the
Python API takes `crf_min` / `crf_max`.

| Encoders | Window | Coarse grid at step 10 |
|---|---|---|
| `libx264`, `libaom-av1`, `libvpx-vp9`, the QSV and VideoToolbox H.264 / HEVC / AV1 adapters | 10..50 | 10, 20, 30, 40, 50 |
| `libx265`, the NVENC and AMF adapters | 15..40 | 15, 25, 35 |
| `libsvtav1` | 20..50 | 20, 30, 40, 50 |
| `libvvenc` | 17..50 | 17, 27, 37, 47 |
| `prores_videotoolbox` (tiers 0..5) | 0..5 | 0, then the fine pass covers 1..5 |

A window the adapter refuses (from the Python API) raises `ValueError`
before the first encode, and the CLI reports any refused `--preset` /
`--crf` cell the same way: one line on stderr and exit status 2. Until
2026-10-04 the window was 10..50 for every encoder, and `libx265`,
`libsvtav1`, `libvvenc`, the AMF adapters and ProRes stopped with a
`ValueError` traceback.

## Hardware encoders

NVENC, AMF and QSV adapters search their own window (see the table
above). The `--crf` value carries the
quality number whether the encoder names it CRF or CQ; the NVENC adapter
forwards it as `-cq`.

NVENC is 10 to 100 times faster than the software encoders at the cost of
quality. Empirically, `h264_nvenc` at `medium` loses 3 to 5 VMAF points
against `libx264 medium` at the same bitrate, depending on content. The Pareto
frontiers differ, which is why the harness lists each hardware encoder as its
own codec rather than as a flag on `libx264`.

| Goal | Use |
| --- | --- |
| A large corpus quickly, or a GPU-encoded production pipeline | A hardware encoder. |
| The best perceptual quality at a given bitrate | A software encoder. |

If FFmpeg reports `Encoder h264_nvenc not found` (or a sibling encoder), the
FFmpeg build lacks `--enable-nvenc` or the GPU generation does not support it.
The harness records the failure as `exit_status != 0` and skips scoring, so a
partial corpus over a mixed fleet stays well-formed. See
[`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) for the adapter
details, including AMF and QSV.

An AMF sweep over three presets and three CRFs:

```shell
vmaf-tune corpus \
    --source ref.yuv --width 1920 --height 1080 \
    --encoder h264_amf \
    --preset slow --preset medium --preset fast \
    --crf 23 --crf 28 --crf 34 \
    --output corpus_amf.jsonl
```

For `--preset medium --crf 23` the adapter emits these encoder arguments:

```shell
ffmpeg -i ref.yuv -c:v h264_amf \
       -quality balanced -rc cqp -qp_i 23 -qp_p 23 \
       -an out.mkv
```

## Timing comparison

The count of visited points is the figure that matters. Wall time per point
varies with source resolution, preset and the libvmaf backend
(`cpu`, `cuda`, `sycl`, `hip` or `metal`), so the figures below are illustrative.

| Mode | Points visited | Relative wall time |
| --- | ---: | ---: |
| Full grid `--crf 0 ... 51` | 52 | 1.00x (baseline) |
| Coarse-to-fine, defaults, target met mid-range | 15 | about 0.29x (3.46x faster) |
| Coarse-to-fine, one-pass shortcut (target met at the coarse maximum) | 5 | about 0.10x (10.4x faster) |
| Coarse-to-fine, target unmet (fine pass runs anyway) | 15 | about 0.29x |

For a 1080p `--preset medium` clip where one encode-plus-score pass takes about
5 s, a recommend run drops from about 260 s to about 75 s.

## Output

Rows are ordinary `vmaf-tune` corpus rows (schema in
[`vmaf-tune-corpus.md`](vmaf-tune-corpus.md)), so
[`recommend`](vmaf-tune-recommend.md), predictor training and the
[bisect](vmaf-tune-bisect.md) tooling read the JSONL without a separate parser.

## History

- ADR-0306 introduced the search in Phase A; the library entry point is
  `tools/vmaf-tune/src/vmaftune/corpus.py::coarse_to_fine_search`, wired in
  `cli.py`.

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — overview of every subcommand.
- [`vmaf-tune-recommend.md`](vmaf-tune-recommend.md) — the target-picking
  consumer of coarse-to-fine rows.
- [`vmaf-tune-bisect.md`](vmaf-tune-bisect.md) — binary search over the CRF
  window, the alternative for one codec and one target.
- [ADR-0306](../adr/0306-vmaf-tune-coarse-to-fine.md) — design decision and
  search strategy.
