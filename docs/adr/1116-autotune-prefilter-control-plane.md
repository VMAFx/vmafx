<!-- markdownlint-disable MD013 -->
# ADR-1116: vmaf-tune autotune prefilter control plane (Pelorus deband)

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: vmaf-tune, autotune, pelorus, filter-adapters, control-plane, optuna, fork-local

## Context

The Pelorus project ships a Vulkan pre-encode debanding filter
(`vf_pelorus_deband_vulkan`) and froze a 10-knob **control-plane
contract** (Pelorus ADR-0110, `docs/api/control-plane.md`) describing
the AVOptions an external optimizer may sweep. The integration plan
(workstreams D1 + D2, `.workingdir2/rc/pelorus/PLAN.md`) asks vmafx to
become that optimizer: drive a VMAF-in-the-loop search over the deband
strengths **and** the encoder's CRF, returning a recommended
strengths + CRF combination.

This is the mode-2 ("control plane") seam from Pelorus ADR-0106. It
works end-to-end today because the deband filter already exposes the
full AVOption surface — no Pelorus-side analyze filter is required.
vmafx must stay **Vulkan-free**: it only emits ffmpeg
`-vf "pelorus_deband_vulkan=..."` strings and scores the encoded output;
the filter itself runs inside ffmpeg.

vmaf-tune already owns the search machinery: codec adapters
(`codec_adapters/`, ADR-0237), an Optuna `TPESampler` study
(`fast.py`, ADR-0276 / ADR-0304), and the encode/score seams
(`encode.py`, `score.py`). The open questions were (1) where a
*pre-filter* — which is not a codec — fits in the adapter taxonomy, and
(2) how to optimise the deband knobs and CRF together without
reinventing the sampler.

## Decision

We add a new **`filter_adapters/`** family, sibling to
`codec_adapters/`, because a pre-encode filter is categorically not a
codec (it has no quality/preset/CRF surface — it has strength knobs and
emits a `-vf` fragment). The first member,
`filter_adapters/pelorus_deband.py`, hard-codes the 10 frozen knobs
(name / type / range / default) verbatim from the ADR-0110 contract and
emits the `-vf` fragment for a parameter dict. A `FilterAdapter`
Protocol mirrors the `CodecAdapter` shape so future pre-filters drop in
as one file without touching the search loop.

We add a **`vmaf-tune prefilter`** subcommand backed by a new
`prefilter.py` module that builds a **joint** Optuna search space — the
10 deband dimensions plus a synthetic `crf` dimension — and runs the
**same `TPESampler` study** `fast.py` uses, minimising
`|achieved_vmaf − target| + λ·kbps`. CRF is suggested as an ordinal
integer alongside the deband knobs in one objective call, so the two
axes are co-optimised rather than searched in nested loops. The live
`deband → encode → score` probe is gated behind a
`pelorus_filter_available()` check; vmafx never runs the filter, only
emits its string.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **New `filter_adapters/` family + joint TPE (chosen)** | Clean taxonomy (filter ≠ codec); reuses the existing TPE study; co-optimises deband+CRF; one-file extensibility | One more adapter family to learn | Selected — matches the codec-adapter precedent and keeps the optimizer codec/filter-agnostic |
| Extend `codec_adapters` with a "filter" pseudo-codec | No new package | Conflates a `-vf` pre-filter with a `-c:v` codec; pollutes the `CodecAdapter` Protocol with filter-only fields | Rejected — a filter has no preset/CRF/two-pass surface; the abstraction would leak |
| Nested search (CRF bisect inside a deband sweep) | Reuses `bisect.py` per deband point | O(knobs × bisect) encodes; ignores deband↔CRF interaction (debanding shifts the rate-quality curve) | Rejected — the interaction is exactly what we want the joint search to exploit |
| Hand-rolled grid over deband knobs | Deterministic | Exponential in 10 knobs; no Bayesian guidance | Rejected — TPE converges in tens of trials vs. a combinatorial grid |

## Consequences

- **Positive**: vmafx can recommend deband strengths + CRF against any
  ffmpeg build that ships the Pelorus filter, today, with no Pelorus
  data-plane dependency. The search space can never silently drift from
  the contract because it is generated from the adapter's frozen knob
  table. The `FilterAdapter` Protocol makes future pre-filters (sharpen,
  denoise) one-file additions.
- **Negative**: the live encode path cannot be exercised in CI or in the
  current dev environment (no pelorus-enabled ffmpeg build present), so
  it is covered only by unit tests with a mocked encode/score loop. The
  joint 11-dimensional search needs more TPE trials than the fast-path's
  single CRF axis (default 60 vs. 30).
- **Neutral / follow-ups**: the contract is a two-repo coupling — if
  Pelorus ADR-0110 changes a knob's range/type/name, this adapter must
  land a matching change in the same coordinated PR pair (a contract
  conformance test, `test_filter_adapter_pelorus_deband.py`, fails on any
  drift to catch it). Workstreams D3 (MCP `autotune_prefilter` tool) and
  D4 (vmafx-server `/v1/autotune-prefilter`) build on this seam.

## References

- Pelorus ADR-0110 control-plane contract: `pelorus/docs/api/control-plane.md`.
- Pelorus ADR-0106 (autotune coupling modes).
- Integration plan: `.workingdir2/rc/pelorus/PLAN.md` (workstreams D1 + D2).
- Reuses the Optuna TPE study from [ADR-0276](0276-vmaf-tune-fast-path.md) / [ADR-0304](0304-vmaf-tune-fast-path-prod-wiring.md).
- Adapter-family precedent: [ADR-0237](0237-quality-aware-encode-automation.md) (codec adapters).
- Research digest: [Research-1116](../research/1116-autotune-prefilter-control-plane.md).
- Source: `req` (user direction — let vmaf-tune run a VMAF-in-the-loop search that drives the Pelorus deband filter's AVOptions plus CRF, returning recommended strengths and CRF).
