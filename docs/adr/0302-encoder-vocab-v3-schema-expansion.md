<!-- markdownlint-disable MD060 -->
# ADR-0302: ENCODER_VOCAB v3 — 16-slot schema expansion + retrain plan

- **Status**: Accepted
- **Date**: 2026-05-05
- **Companion research digest**: [Research-0078](../research/0078-encoder-vocab-v3-schema-expansion.md)
- **Related**: ADR-0235 (codec-aware FRRegressor + 0.95 LOSO PLCC ship gate),
  ADR-0272 (`fr_regressor_v2` smoke scaffold),
  ADR-0291 (`fr_regressor_v2` flip from smoke to production)
- **Re-scope of**: PR #373 (deferred VT-adapters-plus-vocab change; the VT
  adapters landed via a separate PR — see
  `tools/vmaf-tune/src/vmaftune/codec_adapters/h264_videotoolbox.py` /
  `hevc_videotoolbox.py` on master via ADR-0283)

## Context

`fr_regressor_v2` shipped to production in ADR-0291 against
`ENCODER_VOCAB` v2 (13 slots: libx264, libaom-av1, libx265,
h264_nvenc, hevc_nvenc, av1_nvenc, h264_amf, hevc_amf, av1_amf,
h264_qsv, hevc_qsv, av1_qsv, libvvenc). Three vmaf-tune codec
adapters have landed since:

1. `libsvtav1` (PR #294-series, ADR-0294) — software AV1 alongside
   `libaom-av1`, materially different rate-distortion behaviour at
   matched CQ + preset.
2. `h264_videotoolbox` and `hevc_videotoolbox` (ADR-0283) — Apple
   hardware-accelerated H.264 / HEVC adapters; first VT family on
   the corpus side.

The Phase A corpus runner can already emit canonical-6 features for
those three encoders, but `fr_regressor_v2` has never seen them: the
inference path silently maps every unrecognised encoder string to the
`unknown` one-hot column and returns a low-confidence prediction. The
cleanest fix is a vocab bump (v2 → v3) plus a fresh LOSO retrain that
clears the same 0.95 mean-LOSO-PLCC ship gate ADR-0291 cleared.

This ADR documents the schema expansion as a **scaffold-only** change.
The production ONNX swap is gated on a follow-up retrain PR landing
the new checkpoint and clearing the LOSO PLCC ship gate; the in-tree
v2 ONNX continues to serve until then. ADR-0235's append-only invariant
is preserved — every v2 index keeps its column position; the three new
slots append at indices 13/14/15 (one-based: 14/15/16).

## Decision

Land a 16-slot `ENCODER_VOCAB` v3 schema scaffold in
`ai/scripts/train_fr_regressor_v2.py` as a parallel constant
(`ENCODER_VOCAB_V3`), without wiring it into the active training
pipeline. The live `ENCODER_VOCAB` and `ENCODER_VOCAB_VERSION = 2`
remain the source of truth for any retraining run shipping today —
this PR ships **only** the schema definition + the documentation
contract that future v3 retrains MUST satisfy.

**v3 schema** (16 slots, append-only over the user-facing v2 layout
documented in ADR-0291):

| idx | slot | family | new in v3 |
|-----|------|--------|-----------|
|  0  | libx264          | SW H.264 | — |
|  1  | libaom-av1       | SW AV1   | — |
|  2  | libx265          | SW HEVC  | — |
|  3  | h264_nvenc       | NVENC H.264 | — |
|  4  | hevc_nvenc       | NVENC HEVC  | — |
|  5  | av1_nvenc        | NVENC AV1   | — |
|  6  | h264_amf         | AMF H.264   | — |
|  7  | hevc_amf         | AMF HEVC    | — |
|  8  | av1_amf          | AMF AV1     | — |
|  9  | h264_qsv         | QSV H.264   | — |
| 10  | hevc_qsv         | QSV HEVC    | — |
| 11  | av1_qsv          | QSV AV1     | — |
| 12  | libvvenc         | SW VVC      | — |
| 13  | libsvtav1        | SW AV1 (SVT) | **new** |
| 14  | h264_videotoolbox | VT H.264   | **new** |
| 15  | hevc_videotoolbox | VT HEVC    | **new** |

**Backwards-compat strategy.** Until the v3 ONNX ships and clears
the LOSO PLCC ship gate, the runtime continues to load the v2
13-slot ONNX. The v3 schema constant is information-only; no
inference path consumes it yet. Once a follow-up retrain PR clears
the ship gate, that PR (not this one) flips
`ENCODER_VOCAB_VERSION` from 2 to 3, replaces the live `ENCODER_VOCAB`
tuple, registers the new ONNX in `model/tiny/registry.json`, and
documents the v2 → v3 fallback shim removal.

**Ship gate.** Mean LOSO PLCC ≥ 0.95 across all 9 Netflix sources,
matching the gate ADR-0291 cleared. Per ADR-0235, the multi-codec
lift over the v1 single-input regressor must remain ≥ +0.005 PLCC;
that floor was already cleared by v2 and is preserved as the v3
acceptance criterion.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|--------|------|------|---------|
| **16-slot retrain (chosen)** | Single LOSO run covers all three new codecs; preserves append-only invariant; matches ADR-0291 ship-gate cadence | Requires Phase A corpus coverage for SVT-AV1 + VT (the corpus runner already supports them, no blocker) | **Selected** — clears the ship gate in one pass, no schema churn |
| Incremental per-PR retrains (one new slot per PR) | Smallest blast-radius per change; easier bisect if a single codec drags PLCC | 3× the LOSO wall-time + 3× the PR overhead; vocab churn invalidates intermediate ONNX checkpoints; users running `fr_regressor_v2` would see three back-to-back schema bumps | Rejected — cost-of-PR overhead dominates; no real bisect benefit since LOSO already attributes drag per source × encoder cell |
| Deprecate v2 + retrain from scratch (open vocab, no append-only) | Frees the column ordering; lets us drop unused slots | Breaks ADR-0235's append-only invariant; invalidates every shipped `fr_regressor_v2_*.onnx` consumer; forces a v3 majeure version bump on every downstream caller | Rejected — append-only is the contract that lets the ONNX checkpoint freeze across vocab edits; abandoning it for a one-time cleanup costs more than the slot waste saves |
| Defer until a "real" multi-corpus lands | Avoids the risk of OldTownCross-style outliers on the new codecs | Holds back vmaf-tune Phase B consumers that already encode SVT-AV1 + VT material; the Phase A corpus runner can already produce canonical-6 rows for these encoders today | Rejected — the corpus is not the bottleneck, the vocab is; deferring blocks usable predictions on shipped adapters |

## Consequences

- **Visible behaviour** (this PR): zero. The schema scaffold lands as
  a parallel constant; existing v2 inference paths are unaffected.
- **Visible behaviour** (follow-up retrain PR, gated on ship gate):
  `fr_regressor_v2` predictions for SVT-AV1, VT-H.264, and VT-HEVC
  encodes stop falling through to the `unknown` one-hot and start
  receiving codec-aware lift.
- **Backlog opened**: T-FR-V2-VOCAB-V3-RETRAIN — produce Phase A
  corpus rows for libsvtav1 + the two VT encoders, run LOSO,
  retrain, ship if ≥ 0.95 mean LOSO PLCC.
- **No upstream interaction.** `ai/scripts/train_fr_regressor_v2.py`
  is fork-introduced (ADR-0272); upstream Netflix/vmaf has no
  equivalent surface.

## References

- req (2026-05-05, popup re-scope): drop the VT adapters from PR #373
  (already landed via ADR-0283), keep only the 13 → 16 vocab
  expansion + retrain plan; ship as scaffold under a new ADR.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware FR
  regressor + LOSO PLCC ship gate + append-only `CODEC_VOCAB`
  invariant.
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) —
  `fr_regressor_v2` codec-aware smoke scaffold (smoke checkpoint
  shipped pending a real Phase A corpus).
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — flip from smoke to
  production; documents the v2 13-slot vocab and the 0.95 LOSO PLCC
  ship gate this ADR re-uses.
- [ADR-0283](0283-vmaf-tune-videotoolbox-adapters.md) — VT codec
  adapters that motivate slots 14/15.
- [ADR-0294](0294-vmaf-tune-codec-adapter-svtav1.md) — `libsvtav1`
  adapter that motivates slot 13.
- [Research-0078](../research/0078-encoder-vocab-v3-schema-expansion.md)
  — companion research digest with retrain plan, ship gate,
  reproducer.

### Status update 2026-05-09: namespace collision resolved

Two parallel agent reports (`abd6ed552ac8cae60`,
`abda108c8263491da`) surfaced a name collision: a future
"feature-set v3" workstream (canonical-6 + `encoder_internal` +
shot-boundary + `hwcap`) was unintentionally referring to itself
as `fr_regressor_v3` — the same id this ADR's retrain checkpoint
already claims. The collision is resolved per
[ADR-0349](0349-fr-regressor-v3-namespace.md): this ADR's
`fr_regressor_v3` registry row stays bit-identical (sha256
`eaa16d23…`, `smoke: false`) and the future feature-set work
claims the reserved name `fr_regressor_v3plus_features`. No code
change in this ADR; this appendix lands per
[ADR-0028](0028-adr-maintenance-rule.md) (immutability of
Accepted-ADR bodies — append-only status updates only).
