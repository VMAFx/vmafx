### vmafx-tune-go Stage 2 — `ladder` subcommand (per-title ABR bitrate-ladder)

Adds `vmafx-tune-go ladder` — the per-title ABR bitrate-ladder generator,
porting the Python `vmaf-tune ladder` Phase E algorithm to Go.

**What it does:**

- Samples a configurable `(resolution × VMAF-target)` grid using the
  Stage-1 CRF bisect engine (`pkg/bisect`).
- Computes the **upper convex hull** (Pareto frontier) of the sampled
  `(bitrate, vmaf)` cloud.
- Selects **knee renditions** from the hull via Kneedle-style perpendicular-
  distance recursion, capped at `--max-rungs` (default 6).
- Applies a `--min-bitrate-gap` filter (default 100 kbps) to avoid
  adjacent rungs that a player cannot distinguish.
- Emits schema-v1 JSON (superset of Python `ladder.py` output, compatible
  with the existing HLS/DASH manifest renderer) or Markdown.

**Hardware encoder support:** `h264_nvenc`, `hevc_nvenc`, `h264_qsv`,
`hevc_qsv`, `h264_amf`, `hevc_amf`, `libsvtav1`, `libaom-av1` are all
supported via `--codec` when present in the environment.

New package:

- `pkg/ladder/` — `Build`, `upperConvexHull`, `selectRenditions`,
  `applyBitrateGap`, `SamplerFn` seam.
  Unit-testable without ffmpeg or vmaf on PATH.

`ladder` is removed from the stub list in `cmd/vmafx-tune/cmd/root.go`.
The Python `tools/vmaf-tune/` is unchanged.

See [docs/usage/vmafx-tune-go.md](docs/usage/vmafx-tune-go.md) and
[ADR-0730](docs/adr/0730-vmafx-tune-go-stage2.md).
