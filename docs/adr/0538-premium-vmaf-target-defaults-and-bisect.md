<!-- markdownlint-disable MD013 MD060 -->
# ADR-0538: vmaf-tune compare ships premium-archival --target-vmafs default + bisect reaches VMAF 95+

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude
- **Tags**: vmaf-tune, compare, bisect, defaults, premium-archival
- **Supersedes**: target-VMAF defaults from [ADR-0534](0534-compare-rate-quality-chart-from-bisect-samples.md)

## Context

[ADR-0534](0534-compare-rate-quality-chart-from-bisect-samples.md) (PR #1293, just merged) shipped `--target-vmafs 75,80,85,90,93` as the new default for `vmaf-tune compare`, framed as "realistic streaming defaults". For the **fork's actual user** this is the wrong range: archival workflows on this machine encode for VMAF >= 95 exclusively. The streaming/broadcast band (VMAF 75-93) is data the user does not act on; the resulting R-Q chart never includes any of the operating points the user picks CRFs from.

ADR-0534's brief acknowledged the constraint indirectly:

> "The top end stops at 93 because 95+ frequently exceeds the codec's CRF ceiling and produces 'unreachable' failure rows."

That is a **bisect harness bug**, not a property of VMAF or the codecs. CRF 0 is lossless and every modern codec produces VMAF >= 98 at CRF 0 on any reasonable source. The "unreachable" outcome stems from `bisect_target_vmaf` defaulting its search window to the adapter's `quality_range`, which is the perceptually-informative narrow window (e.g. `libx265 = (15, 40)`, `libsvtav1 = (20, 50)`) — too tight to reach VMAF >= 95 on most sources. The adapter's CRF validator additionally rejects any CRF outside that window outright, so even a caller-supplied wider `crf_range` is throttled by the validator inside `_encode_and_score`.

The combined effect is that on a clean BBB 4K v11 sweep (3 codecs x 4 archival targets) every libx265 row at target 96+ comes back ok=false: search window opens at CRF 15, where libx265 already gives VMAF ~94 on BBB, but the bisect then narrows upward (toward higher CRF, away from the only CRFs that could clear the target).

We need to (a) ship a default sweep that matches what the user actually encodes for, and (b) make the bisect physically capable of hitting those targets.

## Decision

### Fix A — premium-archival default

Change the `vmaf-tune compare --target-vmafs` default from `75,80,85,90,93` (ADR-0534) to `94,96,97,98` (four points spanning the user's full archival range). VMAF 94 is the subjectively-transparent floor on 4K source; 98 is the near-lossless ceiling above which marginal CRF reductions cost large bitrate for sub-noise VMAF gains.

### Fix B — high-VMAF bisect contract

Three changes in `tools/vmaf-tune/src/vmaftune/bisect.py`:

1. **Default search window is the encoder absolute range, not the informative window.** Add `_absolute_crf_range(adapter)` returning the encoder's accepted CRF bounds:
   - `libx264, libx265`: `(0, 51)`
   - `libvpx-vp9, libaom-av1, libsvtav1`: `(0, 63)`
   - Other codecs: fall back to `adapter.crf_min/crf_max` if present, then `adapter.quality_range`.

   `bisect_target_vmaf` uses this when `crf_range is None`. Caller-supplied `crf_range` (the `--crf-min` / `--crf-max` CLI knobs, codec-tutorial fixtures) is preserved verbatim — no behaviour change for explicit callers.

2. **Adapter validation bypasses the informative-range CRF check.** The bisect now performs preset validation against `adapter.presets` and CRF validation against `_absolute_crf_range(adapter)` directly, instead of calling `adapter.validate(preset, crf)` (which would reject CRFs outside the informative window). The corpus-generator path in `corpus.py` still calls `adapter.validate` unchanged — only the bisect search loop is widened.

3. **Overshoot at floor is OK.** If the codec already overshoots the target at the lowest accepted CRF (e.g. CRF 0 on libx264 gives VMAF 99.5, target 96 is overshot from the start), the existing bisect logic already converges on the highest CRF that still clears the target — no extra code needed. This ADR documents the contract so future contributors don't mistake the convergence point for an error.

`--max-iterations` default stays at 8 (sufficient for the widest window: `ceil(log2(64)) + 2` safety = 8).

### Fix C — documentation

`docs/usage/vmaf-tune.md` gains a "high-VMAF bisect contract" section describing the three points above, the per-codec absolute ranges, and the rationale for why the default ships premium-archival targets rather than streaming targets.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep ADR-0534 defaults (`75,80,85,90,93`) | No churn; one-PR ago | User never encodes at those targets; report is unactionable | Defeats the purpose of the compare sweep — defaults that don't match real operating points teach the user to ignore the report |
| Ship a `streaming` vs `archival` preset flag (`--target-preset {streaming,archival}`) | Both ranges discoverable; opinionated defaults per use case | Extra surface; user has explicit "I never encode below 95" preference — streaming preset is dead code on this fork | YAGNI for the fork; if a downstream user needs streaming defaults they pass `--target-vmafs 75,80,85,90,93` directly |
| Wider archival sweep (`90,92,94,95,96,97,98,99`) | More data points; smoother R-Q curve | 2x bisect runtime; values <94 are below the user's floor; 99 frequently overshoots even at CRF 0 (no useful information beyond "encoder is lossy") | The four chosen points cover the user's actionable range with minimum wall time |
| **Premium-archival defaults + widen bisect search (chosen)** | Matches the user's stated workflow; bisect actually reaches the targets; no extra CLI surface | One-time churn through ADR-0534's brand-new test fixture | The right answer per req; ADR-0534's bisect-unreachable framing was itself a bug, not a constraint |

## Consequences

- **Positive**: the default `vmaf-tune compare` sweep produces an actionable R-Q chart for the fork's user; codec choice at VMAF 95-98 is now a deterministic harness output rather than a manual `--crf-min 0 --crf-max 12` workaround. Future archival-tuning workflows (per-shot saliency, tiny-AI training) get a calibrated set of premium-tier R-Q probes for free.
- **Negative**: the bisect search window widens by ~30 CRF points for libx265 (15..40 -> 0..51) and ~16 for libsvtav1 (20..50 -> 0..63), so the first iteration's midpoint shifts. Worst-case iteration count is unchanged (still 8) because `ceil(log2(64)) = 6`. The CSV row schema is unchanged; HTML chart x-axis is unchanged (kbps); only the y-axis range commonly extends above 95 now.
- **Neutral / follow-ups**: hardware encoders (NVENC / AMF / QSV / VideoToolbox) and VVenC are not in `_ABSOLUTE_CRF_RANGE_BY_NAME` — they fall through to the adapter's `crf_min/crf_max` or `quality_range`. If a future user reports a hardware-encoder unreachable target, the fix is a single dict entry, not a re-architecture.

## References

- ADR-0534 — establishes the (now-superseded) streaming defaults.
- PR #1293 — ADR-0534 implementation; this ADR reverses its `--target-vmafs` defaults.
- ADR-0516 — multi-target sweep schema that both ADRs build on.
- ADR-0535 — atomic ADR allocator used to claim 0537 without collision.
- `req` (direct user request, 2026-05-18): "I never have encoded stuff below 95. Change defaults to 94,96,97,98 for premium-archival range."
- `req`: "Bisect must reliably hit VMAF 95+ ... at CRF 0 (lossless) every codec produces VMAF >= 98 by definition. The bisect must start its CRF window at the LOWEST CRF the codec accepts."
- BBB 4K v11 report (`.workingdir/bbb_reports/bbb_2160p60_v11_*.{html,md}`) — the libx265 / libsvtav1 unreachable rows at VMAF >= 95 that motivated this ADR.
