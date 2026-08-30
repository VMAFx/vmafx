<!-- markdownlint-disable MD060 -->
# ADR-0446: K150K/CHUG extractor passes HDR and HFR per-feature options

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris, Claude (Anthropic), lawrence (Slack 2026-05-15)
- **Tags**: ai, hdr, hfr, training, corpus, fork-local

## Context

The shared K150K/CHUG feature-extraction script
(`ai/scripts/extract_k150k_features.py`) drove the live 2026-05-15
CHUG MOS-HDR ingest (5 992 clips, ~14 h ETA on RTX 4090). The script
emitted `--feature <name>` arguments to the libvmaf CLI without ever
using the `--feature <name>=k=v:k=v` per-feature options grammar.

(core/tools/cli_parse.c:407-438 implements the parser:
`strsep(&optarg, "=")` consumes the extractor name first, then `:`
separates the `key=value` pairs. The `name=<extractor>:k=v` shape is
NOT part of the grammar and trips
`problem loading feature extractor: name`.)

Two separate audit findings converged on the same root cause:

1. **Issue #837** — lawrence (Slack 2026-05-15) flagged that VMAF
   "struggles like fuck with 50 Hz BBC stuff". Root cause: motion
   features (`motion`, `motion_v2`) measure per-frame absolute
   differences. At 50/60 fps the per-frame delta on the same physical
   motion is ~half what it is at 25/30 fps; at 120 fps it's ~quarter.
   libvmaf has shipped a `motion_fps_weight` per-feature option for
   this since [0d92], but the extractor never set it. CHUG audit
   confirmed 568 of 5 992 ingested clips are >49 fps (14 at 120 fps,
   483 at 59.94 fps), all currently mis-calibrated.

2. **Slack 2026-05-15** — lawrence flagged that the live invocation
   wasn't passing the HDR-aware CAMBI options he'd documented in the
   `hdr_custom_features.py` recipe. CHUG is PQ HDR; CAMBI's default
   `eotf=sdr` uses SDR visibility thresholds, which **under-detect
   banding on PQ content**. MS_SSIM's default `enable_db=true`
   applies a non-linear scale that breaks SVR correlation per the
   recipe.

## Decision

Extend `extract_k150k_features.py` to:

1. **Probe per-clip metadata at extraction time.** `_probe_geometry`
   now returns `(width, height, pix_fmt, fps, color_meta)` with
   `color_meta` carrying `color_primaries`, `color_transfer`,
   `color_space` from `ffprobe`.

2. **Auto-detect HDR.** New `_is_hdr_source(pix_fmt, color_meta)`
   returns true when the source is ≥10-bit AND has a PQ (`smpte2084`)
   / HLG (`arib-std-b67`) transfer characteristic OR BT.2020
   primaries (the weaker fallback when transfer is absent). Missing
   metadata fail-safes to SDR.

3. **Auto-correct motion for HFR.** New `_motion_fps_weight(fps)`
   returns `1.0` for sources in `[24, 32]` fps, otherwise `30 / fps`
   clamped to `[0.25, 4.0]`.

4. **Emit per-feature options through libvmaf's grammar.** New
   `_feature_arg(extractor, is_hdr, motion_fps_weight)` returns
   `<extractor>=k=v:k=v` when any HDR or HFR option applies AND the
   extractor advertises support for it (per the
   `_FEATURE_OPTION_SUPPORT` whitelist below), the bare extractor name
   otherwise:

   - **HDR sources only:**
     - `cambi` — `eotf=pq:full_ref=true`
     - `cambi_cuda` — `eotf=pq` (the CUDA twin's option table omits
       `full_ref`; the whitelist drops it silently rather than tripping
       `problem loading feature extractor`)
     - `float_ms_ssim` — `enable_db=false`
     - `float_ms_ssim_cuda` — bare name (the CUDA twin's option table
       omits `enable_db`; whitelist drops it)
   - **HFR sources only (motion features):**
     - `motion` / `motion_cuda` / `motion_v2` / `motion_v2_cuda` —
       `motion_fps_weight=<value>` (4 decimal places).

   The whitelist is sourced from each extractor's `static const
   VmafOption options[]` table in
   `core/src/feature/{,cuda/}*.c`.

5. **Surface the per-clip metadata in the parquet.** The output dict
   gains `fps`, `is_hdr`, and `motion_fps_weight` columns so trainers
   can stratify by source type and re-extraction post-fix can be
   audited.

6. **Tests.** New `ai/tests/test_extract_k150k_hdr_hfr_options.py`
   covers all detection + emission cases (38 tests, 100 % pass).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-clip auto-detect + emit options (chosen)** | Closes both gaps with one mechanism; works on the existing pipeline shape; backward-compatible (SDR sources unchanged); per-clip metadata surfaced in parquet for trainer stratification | Adds 4 ffprobe fields per clip (zero perf cost — same probe call) | Chosen — minimal scope, maximal coverage |
| **Force HDR options for the entire CHUG run via env var** | Simplest implementation | Mis-applies HDR options to SDR clips in mixed corpora; doesn't address HFR at all; doesn't surface metadata for stratification | Rejected — coarser, breaks SDR clips |
| **Skip the fix; add a downstream feature-set normalisation pass** | No extractor change | Requires re-running the entire CHUG extraction with the same buggy options + then a second pass to "correct" them; also doesn't help live MCP usage | Rejected — wastes the 14 h CHUG run twice |
| **Force `motion_fps_weight=1.0` always; document the HFR caveat in the model card** | Smallest code change | Lawrence's BBC50p complaint goes unaddressed; defeats the point of the libvmaf knob existing | Rejected — the knob exists specifically to fix this |
| **Hand-write per-clip CLI invocations from a Python recipe (lawrence's `hdr_custom_features.py` shape)** | Maximum flexibility per clip | Doesn't fit the existing per-clip script shape; harder to test; harder to thread through the script's worker pool | Rejected — existing script shape with HDR detection achieves the same outcome with less surface |

## Consequences

### Positive

- Lawrence's BBC50p concern (Issue #837) closes for K150K/CHUG-style
  ingestion via the script.
- CAMBI on the 568 HDR clips in the in-flight CHUG run becomes
  PQ-correct after re-extraction.
- The parquet gains `fps` / `is_hdr` / `motion_fps_weight` columns,
  enabling per-source stratification in MOS-head training.
- The same options-emission mechanism extends naturally to other
  future per-feature options (e.g. `vif_kernelscale` for high-detail
  content) without further refactoring.

### Negative

- The in-flight CHUG extraction (started 11:00 local 2026-05-15) is
  producing SDR-calibrated CAMBI / MS_SSIM / motion values for the
  HDR + HFR subset. Once this PR lands, the 568+ affected clips need
  re-extraction (~5 h on the same GPU). Trainers should treat the
  pre-fix parquet as bad-data-on-HDR until the re-extract completes.

### Neutral

- The fix only touches the K150K/CHUG extractor; the libvmaf `vmaf`
  CLI surface itself is unchanged. Direct callers of `vmaf --feature
  cambi` outside this script still get default `eotf=sdr`.
- A separate follow-up will surface the same auto-detection on the
  user-facing `vmaf` CLI via a new `--hdr-aware` flag (tracked but
  out of scope for this PR — the urgent gap is the in-flight
  extractor).

## References

- [Issue #837](https://github.com/VMAFx/vmafx/issues/837) — lawrence's
  BBC50p HFR complaint + the audit-confirmed CAMBI HDR-EOTF gap.
- [`docs/research/0135-hdr-ugc-dataset-license-audit-2026-05-15.md`](../research/0135-hdr-ugc-dataset-license-audit-2026-05-15.md)
  — HDR dataset audit (CHUG inventory + HFR clip count).
- libvmaf `--feature` per-feature-option grammar:
  `core/src/feature/feature_extractor.c::vmaf_feature_dictionary_set`.
- libvmaf CAMBI `eotf` knob: `core/src/feature/cambi.c`.
- libvmaf motion `motion_fps_weight` knob:
  `core/src/feature/integer_motion.c:107` +
  `core/src/feature/integer_motion_v2.c:110`.
- Source: `req` — direct user direction (Slack 2026-05-15: "we do
  extract the full cambi right? like for hdr? rofl … i mean on chug").
