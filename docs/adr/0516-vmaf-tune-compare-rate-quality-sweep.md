<!-- markdownlint-disable MD013 -->
# ADR-0516: `vmaf-tune compare` multi-target rate-quality sweep (schema v2) (2026-05-18)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: vmaf-tune, ux, dx, schema-evolution

## Context

`vmaf-tune compare` shipped with a deliberately minimal surface: one
source, one target VMAF, one row per codec, one chart with two bars +
two dots (bitrate per codec, achieved VMAF per codec). The user
described the resulting chart as a "useless deliverable" — two data
points in two dimensions communicate that two codecs hit roughly the
target, and nothing about how they compare across the operating range
operators actually need to pick between codecs (low / medium / high
quality rungs of an ABR ladder, or a global rate-quality preference).

The original surface also did not surface hardware encoders cleanly.
NVENC / QSV / AMF rows either silently dropped (when the encoder was
not compiled into the host's ffmpeg build) or returned the generic
"encode failed" wording from the bisect — neither distinguishes a
genuine quality regression from "no compatible GPU on this box."

The capability gap is operational rather than algorithmic: the bisect
backend (ADR-0326) already does the right work per (codec, target),
the codec-adapter registry already covers ten encoders, the dev-mcp
container already builds ffmpeg with the full encoder matrix. What was
missing was a CLI shape that exercised the cross-product and a chart
that visualised it.

## Decision

1. **Add a `compare_codecs_sweep` API** that takes a list of target
   VMAFs in addition to the codec list. The thread pool now
   dispatches the full `(codec, target_vmaf)` cross-product (16 jobs
   for a 4-codec / 4-target sweep) rather than one job per codec. Each
   pair gets its own bisect predicate bound to the right target VMAF
   via a per-target memoised factory.

2. **Introduce a v2 JSON schema** stamped with `schema_version: 2` and
   `target_vmafs: [...]`. Row shape is identical to v1 — every row
   carries a `target_vmaf` field — so existing downstream readers that
   only look at the rows list keep working. The schema version + the
   `target_vmafs` list tell the report renderer which view to draw.

3. **Replace the chart for v2 inputs** with a per-codec line plot:
   X-axis bitrate (log scale), Y-axis VMAF achieved, one polyline per
   codec across the (target, bitrate) points, plus a heavier dashed
   line for the pareto frontier (lowest bitrate at each target VMAF).
   The legacy bar+dot chart is retained for v1 ingestion so existing
   compare JSONs do not silently re-render to the new view.

4. **Add a `summary table`** to the report: one row per codec, one
   column per target VMAF, plus an "encode time (ms / frame)" column
   and an "encoder version" column. This is the snapshot a reviewer
   actually wants when deciding between codecs.

5. **Probe hardware encoders before dispatch.** A new
   `probe_encoder_available()` helper grep-matches `ffmpeg -encoders`
   for the encoder name (catches "not compiled into ffmpeg") and runs
   a 1-frame lavfi dummy encode (catches "no compatible GPU at
   runtime"). Encoders that fail the probe surface as `ok=false` rows
   with a stable `hardware encoder not available: …` error string —
   the renderer flags them visually without aborting the sweep.

6. **Default `--encoders` to the CPU encoder set**
   `libx264,libx265,libsvtav1,libvpx-vp9` so the basic "what's the
   best codec for this source?" question now has a one-flag answer.
   Hardware encoders remain opt-in via explicit `--encoders`.

7. **Add `--target-vmafs`** as the canonical multi-target flag.
   `--target-vmaf` (singular) is preserved for back-compat as
   syntactic sugar for the legacy single-target path; passing only
   `--target-vmaf` continues to emit the v1 JSON shape so existing
   automation does not break.

## Schema migration

v1 (single-target legacy):

```json
{
  "src": "ref.yuv",
  "target_vmaf": 92.0,
  "rows": [{"codec": "libx264", "bitrate_kbps": 2400.0, ...}]
}
```

v2 (multi-target sweep, ADR-0516):

```json
{
  "schema_version": 2,
  "src": "ref.yuv",
  "target_vmafs": [85.0, 90.0, 92.0, 95.0],
  "rows": [
    {"codec": "libx264", "target_vmaf": 85.0, "bitrate_kbps": 1200.0, ...},
    {"codec": "libx264", "target_vmaf": 90.0, "bitrate_kbps": 2400.0, ...},
    ...
  ]
}
```

The discriminator is `schema_version >= 2 OR "target_vmafs" in payload`;
v1 ingestion continues to recognise the existing `rows` / `results`
keys for downstream consumers that already read those.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| **Multi-target sweep + line chart (this ADR)** | Operators can pick a codec from one glance at the rate-quality curve; pareto frontier highlights the right choice per quality rung; same number of CLI invocations as the legacy single-target run | Schema change; bisect work scales linearly with target count | **Accepted** |
| Stacked bar chart (one bar per (codec, target)) | Re-uses the existing chart code | Still no curve; visual clutter scales O(codecs × targets); does not surface the cross-codec frontier | Rejected |
| Per-target separate runs + manual diff | Zero code change | Defers the work to the operator every time; nothing to PR; defeats the point of `compare` as a one-shot subcommand | Rejected |
| Multi-target only, drop v1 | Smaller code surface (no schema discriminator) | Breaks every existing automation that consumes the legacy JSON; v1 has been in tree since PR #435 | Rejected |
| Schema v2 in a sibling subcommand (`compare-sweep`) | Avoids any back-compat surface in `compare` | Two near-identical subcommands; doc burden doubled; users have to remember which one renders the curve | Rejected |

## Consequences

- `vmaf-tune compare --target-vmafs 85,90,92,95 --encoders ...` now
  emits the rate-quality sweep schema and `vmaf-tune report` renders
  the curve chart with pareto-frontier highlight + summary table.
- Hardware encoders the host can't run no longer crash a comparison —
  they render as a visually-flagged skip row.
- The default `--encoders` set means `vmaf-tune compare --src foo --target-vmafs 85,90,92,95`
  is now the "minimum viable" invocation for the basic codec-pick question.
- Downstream consumers of the v1 JSON keep working; only the chart
  view depends on the discriminator.
- New bisect work scales linearly with `len(target_vmafs)`. A 4-codec
  / 4-target sweep at 8 max-iterations per bisect is ~16x the single-
  target legacy run; `--max-workers` and the per-codec thread pool
  amortise this on multi-core hosts.

## References

- req (paraphrased from user): "Right now compare runs 2 codecs at 1
  target VMAF and renders a bar chart with 2 bars + 2 dots — a useless
  deliverable. Make it a rate-quality curve per codec across multiple
  VMAF targets, log-scale bitrate axis, pareto frontier highlighted,
  plus a summary table with bitrate at VMAF-{85,90,92,95}. Hardware
  encoders gated with availability probing."
- ADR-0326: `vmaf-tune` Phase B target-VMAF bisect predicate.
- ADR-0498: Compare-JSON v1 shape and ok / degraded aggregation.
- ADR-0509: `vmaf-tune compare` container-source framerate auto-probe.
- ADR-0511: MCP backend-probe ordering — sibling consumer of the same
  per-target bisect predicate.
