# `vmaf-tune` Phase E — Bitrate Ladder

`vmaf-tune ladder` builds a per-title ABR ladder from sampled
`(resolution, target-VMAF)` cells. It scores the sampled points,
constructs the Pareto frontier, picks a bounded number of rungs, and
emits an HLS, DASH, or JSON manifest.

The implementation lives in `tools/vmaf-tune/src/vmaftune/ladder.py`
and the CLI entry point is wired in `tools/vmaf-tune/src/vmaftune/cli.py`.

## Quick Start

```shell
vmaf-tune ladder \
    --src ref.yuv \
    --encoder libx264 \
    --resolutions 1920x1080,1280x720,854x480 \
    --target-vmafs 95,92,88 \
    --quality-tiers 5 \
    --format hls \
    --output master.m3u8
```

## Pipeline

1. `build_ladder()` samples every `(resolution, target_vmaf)` pair.
2. The default sampler runs the canonical 5-point CRF sweep and picks
   the row closest to the target.
3. `convex_hull()` removes dominated bitrate/quality points.
4. `select_knees()` chooses the requested number of rungs along the
   hull.
5. `emit_manifest()` writes HLS, DASH, or JSON.

Programmatic callers can pass `sampler=` to `build_ladder()` for a
custom grid, a precomputed corpus, or a true bisect-backed sampler.

## CLI Flags

| Flag | Default | Notes |
| --- | --- | --- |
| `--src PATH` | — | Source clip. |
| `--encoder NAME` | `libx264` | Any registered codec adapter. |
| `--resolutions LIST` | — | Comma-separated `WxH` list. |
| `--target-vmafs LIST` | — | Comma-separated VMAF targets. |
| `--quality-tiers N` | `5` | Number of final rungs. |
| `--format` | `hls` | `hls`, `dash`, or `json`. |
| `--spacing` | `log_bitrate` | Knee spacing strategy: `log_bitrate` or `uniform`. |
| `--with-uncertainty` | off | Apply conformal-interval rung pruning/insertion when interval data is present. |
| `--output PATH` | stdout | Manifest destination. |

## See Also

- [`vmaf-tune-ladder-default-sampler.md`](vmaf-tune-ladder-default-sampler.md)
  — default sampler details.
- [`vmaf-tune.md`](vmaf-tune.md) — base tool and examples.
- [ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md) — Phase-E
  design.
- [ADR-0307](../adr/0307-vmaf-tune-ladder-default-sampler.md) — default
  sampler decision.
