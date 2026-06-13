# ADR-0288: `vmaf-tune` libx265 codec adapter

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, automation, fork-local, vmaf-tune

## Context

[ADR-0237](0237-quality-aware-encode-automation.md) committed
`tools/vmaf-tune/` to a multi-codec design from day one but Phase A
shipped only the `libx264` adapter. A follow-up audit on PR #354
flagged "codec adapter coverage" as the biggest blocker for the four
roadmap buckets that need cross-codec data: bitrate-ladder (#6),
codec-comparison (#7), HDR (#9), and Pareto (#15). Without `libx265`,
`libsvtav1`, and `libaom`, the harness stays single-codec and the
ADR-0235 codec-aware FR regressor cannot get the corpus rows it
needs to disambiguate codec identity from content / quality
covariates.

x265 is the natural first follow-up after x264: same FFmpeg invocation
shape (`-c:v libx265 -crf … -preset …`), same 0..51 CRF axis, the
deepest deployment leverage among non-AV1 modern codecs, and an
encode time profile (~5–20× x264 for the same preset) that keeps
corpus generation tractable on dev hardware.

## Decision

We will ship `tools/vmaf-tune/src/vmaftune/codec_adapters/x265.py` as
a one-file addition mirroring the `x264.py` shape: a frozen dataclass
that declares the codec metadata (`name`, `encoder`, `quality_knob`,
`quality_range`, `quality_default`, `invert_quality`, `presets`) plus
`validate(preset, crf)` and `profile_for(pix_fmt)` methods. The
registry in `codec_adapters/__init__.py` registers the adapter under
the key `libx265`. The shared ffmpeg driver in `encode.py` gains an
encoder-aware version-banner regex so corpus rows record
`libx265-<version>` correctly. The `--encoder` CLI flag continues to
gate the choice via `argparse choices=list(known_codecs())`. No
schema bump is required — the existing `encoder` row column already
carries codec identity.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **One-file adapter mirroring `x264.py` (chosen)** | Smallest diff; preserves the ADR-0237 "no-special-casing in the search loop" invariant; no registry refactor | Adds a per-codec banner regex to `encode.py` (one branch in `parse_versions`) | Picked: matches the AGENTS.md "new codecs are one-file additions" invariant the Phase A scaffold pinned |
| Single multi-codec adapter (one `FFmpegAdapter` parameterised by encoder name) | Less code per codec | Loses per-codec validation (preset names, profile mapping, quality default); regresses the ADR-0237 codec-adapter contract | Rejected: the contract is the seam; collapsing it forces the search loop to learn codec-specific behaviour |
| Defer x265 until a real corpus is captured | Smaller surface | Holds ADR-0235 codec-aware regressor blocked indefinitely; PR #354 audit explicitly named this the top blocker | Rejected: adapter availability is the prerequisite, not the consequence |
| Ship libsvtav1 first | Higher long-term leverage (AV1 deployment grows) | 100× encode time on dev hardware; SVT-AV1 preset numbering is integer 0..13 (different shape from x264/x265) and warrants its own ADR | Rejected: x265 is the cheap unblock; AV1 follows in a sibling PR |

## Consequences

- **Positive**:
  - Unblocks the next set of `vmaf-tune` corpus rows (codec ∈ {x264,
    x265}) — the minimum input the ADR-0235 codec-aware regressor needs
    to start training a codec-discriminating signal.
  - Pins the per-codec adapter shape with a second concrete instance,
    making it harder for a third-codec PR to drift the contract.
  - Adds the `profile_for(pix_fmt)` helper as the canonical place to
    map `yuv420p10le` → `main10`; downstream HDR work (#9) consumes
    this without re-implementing the table.
- **Negative**:
  - `parse_versions` now branches on `encoder`; future codecs add one
    branch each. Tolerable up to ~6 codecs; a registry-driven dispatch
    becomes worth doing if the count climbs further.
  - Real-binary integration coverage is gated on the runner having
    `ffmpeg` built with `--enable-libx265`; we ship the test as a
    `VMAF_TUNE_INTEGRATION=1`-skipped case.
- **Neutral / follow-ups**:
  - `libsvtav1` adapter is the next sibling PR; AGENTS.md row pre-empts
    the rebase concern.
  - Schema bump deferred until Phase B introduces non-CRF quality
    knobs (e.g. SVT-AV1's `--qp` axis under two-pass).

## References

- Parent ADR: [ADR-0237](0237-quality-aware-encode-automation.md).
- Companion: [ADR-0235](0235-codec-aware-fr-regressor.md) consumes the
  multi-codec corpus rows this PR unblocks.
- Source: `req` — PR #354 audit named "codec adapter coverage" the
  top blocker for buckets #6, #7, #9, #15; the user requested the
  x265 adapter as the first sibling-codec PR after Phase A.
- No research digest needed: trivial one-file mirror of an existing
  adapter; option matrix is exhausted in §Alternatives considered.
