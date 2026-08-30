- `vmaf-tune compare` rate-quality chart (multi-target sweeps, v2
  schema) now renders genuine per-codec R-Q curves from every probe
  the underlying target-VMAF bisect computed, instead of connecting
  the picked-CRF cells across (codec, target) pairs. Previously the
  chart connected (codec, target_85) → (codec, target_90) →
  (codec, target_92) → ... by line; because the bisect overshoots
  each target by a different amount, the achieved-VMAF line could
  show physically impossible downward dips (e.g. BBB 4K v10: libx265
  target 92 → achieved 90.5, then target 95 → achieved 95.3, drawing
  a downward slope that read as "more bitrate, less quality").
  The new chart aggregates `bisect_samples` per codec, dedupes by
  CRF, sorts by bitrate, and plots a monotonic R-Q curve with the
  picked-CRF rows highlighted as larger circled markers on top. The
  pareto frontier stays as the dashed overlay. Old v2 JSON dumps
  without `bisect_samples` (and v1 single-target JSONs) still render
  via the legacy connect-the-dots line plus a caveat note in the
  title. Schema v2 is additive: the new `bisect_samples` row field
  is optional, CSV intentionally drops the structured column.
  (ADR-0530, ADR-0516).
- `vmaf-tune compare --target-vmafs` default flipped from
  `85,90,92,95` to `75,80,85,90,93`. The old default was a
  premium-archival sweep that ignored the broadcast / low-bandwidth
  streaming operating range (VMAF 70-90) and frequently produced
  "unreachable" failure rows at 95+ because the codec's CRF ceiling
  cannot push 4K high-motion content past VMAF 94-95. The new
  default covers realistic streaming targets in one shot. Back-compat
  preserved: `--target-vmaf NN` alone (without `--target-vmafs`)
  still emits the v1 single-target schema via a `_TrackedDefaultAction`
  sentinel that detects which flag the user passed explicitly.
  (ADR-0530).
