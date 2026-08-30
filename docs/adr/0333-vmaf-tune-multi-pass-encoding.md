<!-- markdownlint-disable MD060 -->
# ADR-0333: `vmaf-tune` Phase F — multi-pass encoding (libx265 first)

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, automation, fork-local, vmaf-tune, phase-f

## Context

`tools/vmaf-tune/` shipped Phases A–E single-pass throughout: every
encode the harness drives is one ffmpeg invocation that takes the
source YUV, the codec adapter's CRF / preset, and writes the
distorted output. That covers the common per-shot / per-title quality
case, but it leaves a known compression-efficiency lever on the table.

A *2-pass* encode (sometimes "multi-pass") runs the encoder twice
over the source: pass 1 analyses the input and emits a stats file;
pass 2 reads that stats file to make better rate-allocation decisions
(typically targeting an average bitrate, ABR). On libx265 the
documented gain at fixed bitrate is on the order of +0.3 to +0.8 dB
PSNR / +1–3 VMAF points vs single-pass ABR, depending on content
complexity and target bitrate (see Research-0091, x265 docs §rate
control). Multi-pass is the **default** in production VOD workflows
(Netflix per-title, YouTube, Vimeo) for exactly this reason: when
you have an offline budget and a bitrate target rather than a quality
target, the second pass pays for itself.

vmaf-tune today is CRF-driven (constant quality), where 2-pass is
not a meaningful win — CRF already adjusts QPs frame-by-frame from
the first pass's lookahead. The gap shows up the moment a caller
wants **target-bitrate** rather than target-VMAF: the Phase E ladder
(ADR-0295, ADR-0307) emits per-rung bitrate targets, and the
codec-comparison flow (ADR-0290) ranks codecs at a fixed bitrate.
Both currently force-fit a CRF that approximates the requested
bitrate, which leaves quality on the table that a real 2-pass ABR
encode would capture.

Phase F lights up 2-pass encoding for the codecs that benefit, in
opt-in fashion (default behaviour stays single-pass CRF). The first
PR (this one) ships the **architectural seam** plus the canonical
implementation on `libx265` — the encoder where 2-pass is most
faithfully documented and most widely deployed. Sibling codecs follow
in one-file PRs once the seam is pinned.

### Codec landscape — which codecs benefit from 2-pass?

The decision of which codec adapters to wire 2-pass for is informed
by published guidance and the encoder's documented modes:

| Codec | 2-pass benefits? | Notes |
|---|---|---|
| `libx265` | **Yes** — flagship | Native `--pass 1` / `--pass 2 --stats <file>` via `-x265-params`; documented +1–3 VMAF at target bitrate vs 1-pass ABR. **Phase F first implementation.** |
| `libx264` | **Yes** | Native `-pass 1 -passlogfile <prefix>` / `-pass 2 -passlogfile <prefix>`; well-understood for VOD. Phase F sibling PR. |
| `libsvtav1` | **Yes (3-pass too)** | SVT-AV1 supports 1/2/3-pass; the third pass is a refinement of the second. Adapter argv shape differs (`-svtav1-params passes=2`). Phase F sibling PR. |
| `libvvenc` | **Yes** (2-pass via `-pass`) | Fraunhofer VVenC supports 2-pass; argv path needs `vvenc-params` plumbing. Phase F sibling PR. |
| `libaom-av1` | **Yes** | `-cpu-used` axis encoder; native 2-pass via `-pass 1 -passlogfile`. Phase F sibling PR but lower priority (encode time prohibitive). |
| `hevc_nvenc` / `h264_nvenc` / `av1_nvenc` | **Yes (limited)** | NVENC supports `-multipass {disabled,qres,fullres}` (single-invocation lookahead, not a true two-call multi-pass). The semantics differ enough to deserve a separate adapter contract; Phase F-NVENC is a follow-up ADR. |
| `*_amf` (AMD) / `*_qsv` (Intel) / `*_videotoolbox` (Apple) | **No (or no-op)** | Hardware encoders generally do not expose a stats-file 2-pass; they have internal lookahead instead. The adapter contract returns "not supported" so callers don't accidentally run a two-call sequence that produces no quality delta and doubles encode time. |

**This PR ships only `libx265`.** Sibling adapters land in one-file
PRs that mirror this PR's seam (the `two_pass_args(pass_number, stats_path)`
adapter method and the `EncodeRequest.pass_number` / `stats_path` fields).

## Decision

We will:

1. **Extend the adapter contract** with an optional
   `two_pass_args(pass_number: int, stats_path: Path) -> tuple[str, ...]`
   method. Default base implementation (in adapters that do not
   support 2-pass) returns `()` and signals "single-pass only" via a
   `supports_two_pass: bool = False` class attribute. Adapters that
   support 2-pass override `supports_two_pass = True` and emit the
   correct argv slice for pass 1 and pass 2.
2. **Implement `libx265` first** — `X265Adapter.supports_two_pass = True`,
   `two_pass_args(pass_number, stats_path)` returns the right
   `-x265-params pass=N:stats=<path>` argv to layer onto the existing
   single-pass invocation. (libx265's 2-pass switches go through
   `-x265-params`, not the standalone `-pass`/`-passlogfile` ffmpeg
   flags that x264 uses.)
3. **Extend `EncodeRequest`** with optional `pass_number: int = 0`
   (0 = single-pass; 1 / 2 = pass index) and `stats_path: Path | None
   = None`. `build_ffmpeg_command` consumes these by calling the
   adapter's `two_pass_args` and splicing the result before the
   per-codec `extra_params`. Pass 1 redirects output to `-f null -`
   (avoiding writing a useless pass-1 mp4) when `pass_number == 1`;
   pass 2 writes the actual output as today.
4. **Add a thin `run_two_pass_encode(req, ...)` helper** in
   `encode.py` that runs pass 1 followed by pass 2 with a
   per-encode unique stats-file path under
   `tempfile.mkdtemp(prefix="vmaftune-2pass-")`, cleans up on
   completion, and returns a single `EncodeResult` representing the
   combined operation (encode time = sum of both passes; output size
   = pass-2 output size; encoder version = pass-2 stderr; exit status
   = first non-zero of {pass 1, pass 2}).
5. **Wire a CLI flag `--two-pass`** that opts into this path on the
   `corpus` and `recommend` subcommands. Default is **off**. When set
   against an adapter where `supports_two_pass = False`, vmaf-tune
   writes a one-line stderr warning and falls back to single-pass
   (rather than failing) — same precedent as the existing saliency
   "x264-only, fallback to plain encode" path.
6. **Cache key includes pass count** (ADR-0298). The
   content-addressed encode cache must not return a single-pass
   encode when a 2-pass encode is requested. The cache key gains a
   `two_pass: bool` field; a 1-pass key and a 2-pass key for the
   same (src, codec, preset, crf) are distinct.
7. **Sample-clip mode (ADR-0297) composes with 2-pass.** Both passes
   apply the same `-ss <start> -t <N>` input slice; the stats file
   is unique per encode request (and thus per slice). No ordering
   concerns.
8. **Per-shot loop (ADR-0264) and ladder loop (ADR-0295) inherit
   2-pass** transparently the moment they pass `pass_number=1/2` (or
   `--two-pass` from the CLI) through to `run_encode`. This PR does
   not flip the per-shot / ladder default; the seam is what's pinned.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Single adapter method `two_pass_args(N, stats)` + `supports_two_pass` flag (chosen)** | Keeps the per-codec contract narrow; one method per codec; the search loop never branches on codec name | One more method on the contract | Picked: matches the ADR-0288 / ADR-0237 "one-file additions" invariant; the seam stays uniform across codec adapters. |
| Drive 2-pass from a separate `multi_pass_encode.py` module branching on codec name | Self-contained; no contract growth | Reintroduces the codec-name branch the registry is meant to eliminate; regresses ADR-0237 | Rejected: explicitly the anti-pattern AGENTS.md pinned. |
| Flip 2-pass to default-on for codecs that support it | Best quality out of the box | Doubles encode time silently; the corpus-row schema would need a per-row `pass_count` to keep historical rows comparable; users who built timing budgets around single-pass would see surprise regressions | Rejected: opt-in keeps the default invariant intact. We can revisit once a corpus-row schema bump and a timing-budget audit land. |
| Implement 2-pass on x264 first instead of x265 | x264 is the Phase A canonical adapter | x264's `-pass`/`-passlogfile` is also fine but x265's `-x265-params pass=N:stats=...` is the modern shape (the same shape libsvtav1 / libvvenc use via their own `-*-params` channels). Pinning the seam against the modern shape first keeps the sibling-PR template clean. | Rejected (in this PR): x264 follows next as a sibling PR. |
| Skip Phase F entirely; tell users to run 2-pass externally | No code | Loses the integration with the cache (ADR-0298), the score backend, and the per-shot/ladder loops; users can't compare 1-pass vs 2-pass corpora because the harness can't produce the 2-pass rows | Rejected: the integration is the whole point of vmaf-tune. |

## Consequences

- **Positive**:
  - Phase F seam pinned with one concrete codec implementation;
    sibling codec adapters (libx264, libsvtav1, libvvenc) become
    one-file follow-up PRs.
  - Unblocks honest 1-pass vs 2-pass corpus rows for the
    Phase E target-bitrate ladder and the codec-comparison flow at
    fixed bitrate.
  - Cache key extension keeps the content-addressed cache
    (ADR-0298) honest across pass counts; no chance of a 1-pass
    encode being silently returned for a 2-pass request.
- **Negative**:
  - 2-pass doubles encode wall time per cell. The default stays
    off; callers who opt in are aware.
  - Hardware-encoder NVENC's `-multipass` is *not* covered by this
    seam — it's a single-invocation lookahead axis, not a stats-file
    two-call sequence. The adapter contract reserves
    `supports_two_pass = False` for those; an NVENC-specific
    multipass axis is a follow-up ADR if the demand surfaces.
  - One narrow exception in `encode.run_encode` learns about the
    pass-1 `-f null -` output redirect. The branch is in
    `build_ffmpeg_command` (not the search loop), so the
    ADR-0237 "no codec-name branches in the loop" invariant still
    holds.
- **Neutral / follow-ups**:
  - Sibling codec adapters land one-PR-at-a-time per ADR-0288
    pattern.
  - Corpus row schema bump (`pass_count: int`) deferred until the
    second 2-pass-capable adapter lands; today's row stays
    backward-compatible because the field would default to `1`.

## References

- Parent ADR: [ADR-0237](0237-quality-aware-encode-automation.md) —
  multi-codec adapter contract.
- Sibling: [ADR-0288](0288-vmaf-tune-codec-adapter-x265.md) — the
  libx265 adapter this PR extends.
- Cache key extension: [ADR-0298](0298-vmaf-tune-cache-key.md) — the
  pass-count field lands as a non-breaking extension.
- Sample-clip composition: [ADR-0297](0297-vmaf-tune-sample-clip.md).
- Phase E target-bitrate ladder: [ADR-0295](0295-vmaf-tune-bitrate-ladder.md),
  [ADR-0307](0307-vmaf-tune-ladder-default-sampler.md) — 2-pass is the
  natural input once `--target-bitrate` paths land.
- Source: `req` — user requested Phase F design + first PR
  proof-of-concept on `libx265` (the canonical 2-pass implementation).
- No research digest needed: trivial — option matrix is exhausted in
  §Alternatives considered, and the encoder docs (x265 `--pass`,
  ffmpeg `-x265-params`) are the load-bearing reference.
