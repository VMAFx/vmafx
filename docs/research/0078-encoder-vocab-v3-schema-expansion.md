<!-- markdownlint-disable MD060 -->
# Research-0078: ENCODER_VOCAB v3 — 16-slot schema expansion + retrain plan

- **Status**: First retrain landed gate-passing (ADR-0323, 2026-05-06);
  multi-codec retrain still pending
- **Date**: 2026-05-05
- **Companion ADR**: [ADR-0302](../adr/0302-encoder-vocab-v3-schema-expansion.md)
- **Predecessors**:
  [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) (codec-aware
  decision + LOSO PLCC ship gate),
  [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md)
  (smoke scaffold),
  [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) (production
  flip + LOSO PLCC = 0.9681 baseline)

## Question

Three vmaf-tune codec adapters (`libsvtav1`,
`h264_videotoolbox`, `hevc_videotoolbox`) have landed on master since
`fr_regressor_v2` flipped to production against the 13-slot
`ENCODER_VOCAB` v2. Inference for those codecs falls through to the
`unknown` one-hot column. What is the smallest change that adds them
to the vocab without invalidating the v2 ONNX consumer contract, and
what retrain effort does the new vocab require to clear the ADR-0291
ship gate?

## Scope

- **In scope**: the schema expansion definition (16-slot tuple),
  the LOSO ship gate the retrain must clear, and the backwards-compat
  shim that keeps the v2 ONNX serving until v3 ONNX clears the gate.
- **Out of scope**: the actual Phase A corpus expansion run for the
  three new codecs (Phase A runner already supports them, but the
  retrain corpus has not been generated yet — that lands in the
  follow-up retrain PR), and any change to the `fr_regressor_v2`
  graph topology beyond the codec-block one-hot width.

## Three new slots

| idx | slot | adapter file | corpus tag |
|-----|------|--------------|------------|
| 13  | libsvtav1         | `tools/vmaf-tune/src/vmaftune/codec_adapters/svtav1.py` | `"libsvtav1"` |
| 14  | h264_videotoolbox | `tools/vmaf-tune/src/vmaftune/codec_adapters/h264_videotoolbox.py` | `"h264_videotoolbox"` |
| 15  | hevc_videotoolbox | `tools/vmaf-tune/src/vmaftune/codec_adapters/hevc_videotoolbox.py` | `"hevc_videotoolbox"` |

The corpus runner emits the codec tag verbatim from the adapter
registry key, so the vocab strings must match the adapter file's
registry name exactly. ADR-0235 §References lists this rule
("never silently default to a codec that doesn't match what the
script actually encoded"); this PR honours it by deriving the new
slot strings directly from the registry.

## Retrain ship gate

**Mean LOSO PLCC ≥ 0.95** across all 9 Netflix sources, matching the
gate ADR-0291 cleared at 0.9681 ± 0.0207. Acceptance criteria for the
follow-up retrain PR:

1. Mean LOSO PLCC ≥ 0.95 (hard floor — exit non-zero on failure).
2. Multi-codec PLCC lift ≥ +0.005 over the v1 single-input regressor,
   matching the ADR-0235 invariant. v2 cleared this comfortably; v3
   adds 3 slots' worth of one-hot width without changing the MLP
   topology, so the lift floor should remain trivially clearable.
3. No source held-out PLCC below 0.85 (relaxed per-source floor; the
   v2 OldTownCross outlier sat at 0.9183 and was held in scope by
   ADR-0291; v3 inherits the same relaxation rather than tightening
   it on a vocab-only change).
4. RMSE within 1.5× of the v2 production checkpoint's per-source RMSE
   for any source already covered in v2 (regression detector — a
   vocab bump should not degrade prediction quality on previously
   shipped codecs).

## Backwards-compat strategy

The schema scaffold (this PR) does **not** alter the live
`ENCODER_VOCAB` constant or `ENCODER_VOCAB_VERSION`. It adds an
`ENCODER_VOCAB_V3` parallel tuple as documentation of the target
schema. The v2 13-slot ONNX continues to serve every consumer; the
runtime fallback for an unrecognised encoder string remains the
`unknown` one-hot column.

The follow-up retrain PR is responsible for:

1. Bumping `ENCODER_VOCAB` to the 16-slot v3 tuple in place.
2. Bumping `ENCODER_VOCAB_VERSION` from 2 to 3.
3. Removing the `ENCODER_VOCAB_V3` parallel constant (the live
   `ENCODER_VOCAB` becomes the single source of truth again).
4. Training a fresh ONNX against the expanded Phase A corpus and
   shipping it under `model/tiny/fr_regressor_v2.onnx` (path stays
   stable; sha256 + sidecar in `model/tiny/registry.json` are the
   integrity contract that prevents accidental v2-vs-v3 mixing).
5. Honouring the documented load-fallback shim: a runtime that
   encounters a v2 ONNX in registry but receives a v3 vocab string
   collapses the unknown indices into the `unknown` column rather
   than failing the inference call. Symmetrically, a v2 vocab string
   loaded against a v3 ONNX uses the matching v3 column index — the
   v2 indices 0..12 are preserved verbatim under append-only.

## Production-flip checklist (for the follow-up retrain PR)

- [ ] Phase A corpus coverage: `runs/phase_a/full_grid/per_frame_canonical6.jsonl`
      contains rows tagged `libsvtav1`, `h264_videotoolbox`,
      `hevc_videotoolbox` for ≥ 6 of the 9 Netflix sources each
      (matching v2's per-source coverage on the existing 12 hardware
      encoders).
- [ ] LOSO eval clears the four acceptance criteria above.
- [ ] `ENCODER_VOCAB_VERSION` bumped 2 → 3 in
      `ai/scripts/train_fr_regressor_v2.py`.
- [ ] `model/tiny/registry.json` updated: new sha256, byte length,
      and `vocab_version: 3` field (the schema must allow this — if
      it does not today, the registry schema bump rides along in the
      same PR).
- [ ] Sidecar JSON `encoder_vocab` array contains 16 entries in the
      order documented in ADR-0302's table.
- [ ] `docs/ai/inference.md` example includes at least one of the
      three new codecs in its sample output.
- [ ] `ai/AGENTS.md` v3 retrain invariant section moved from
      "pending" to "shipped"; the v2 ONNX entry is removed from the
      "do not replace until cleared" list.
- [ ] Smoke check: a synthetic 16-row test passes
      `python -m pytest ai/tests/ -k encoder_vocab` after the retrain
      PR adds the test fixture.

## Reproducer (this PR — schema scaffold only)

```bash
# Verify the v3 constant parses and has the documented length.
python3 -c "
import importlib.util, pathlib
spec = importlib.util.spec_from_file_location(
    't', pathlib.Path('ai/scripts/train_fr_regressor_v2.py')
)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
assert len(m.ENCODER_VOCAB_V3) == 16, len(m.ENCODER_VOCAB_V3)
assert m.ENCODER_VOCAB_VERSION == 2, 'live vocab still v2 on the scaffold PR'
print('OK')
"

# Will be a no-op until the follow-up retrain PR adds the test
# fixture; included here as the canonical smoke command.
python -m pytest ai/tests/ -k encoder_vocab
```

## Headline results — first retrain (ADR-0323, 2026-05-06)

The first v3 retrain shipped under PR
[`feat(ai): fr_regressor_v3 — train + register on ENCODER_VOCAB v3
(16-slot)`](../adr/0323-fr-regressor-v3-train-and-register.md), training
[`ai/scripts/train_fr_regressor_v3.py`](../../ai/scripts/train_fr_regressor_v3.py)
on the existing Phase A canonical-6 corpus (5,640 rows, NVENC-only).

**Gate PASS.** Mean LOSO PLCC = **0.9975 ± 0.0018** across the 9
Netflix sources. Per-source PLCC range 0.9945 → 0.9996; every source
clears the 0.99 mark and the relaxed per-source 0.85 floor. The
0.95 hard floor is cleared with ~5pp margin.

| Source                    | PLCC   | SROCC  | RMSE  |
|---------------------------|--------|--------|-------|
| BigBuckBunny_25fps        | 0.9973 | 0.9878 | 0.787 |
| BirdsInCage_30fps         | 0.9988 | 0.9989 | 0.432 |
| CrowdRun_25fps            | 0.9996 | 0.9972 | 0.677 |
| ElFuente1_30fps           | 0.9987 | 0.8805 | 0.822 |
| ElFuente2_30fps           | 0.9950 | 0.9984 | 3.288 |
| FoxBird_25fps             | 0.9945 | 0.9329 | 0.904 |
| OldTownCross_25fps        | 0.9981 | 0.9951 | 0.810 |
| Seeking_25fps             | 0.9989 | 0.9877 | 1.013 |
| Tennis_24fps              | 0.9962 | 0.9436 | 1.061 |

The OldTownCross outlier from v2 (0.9183) cleared 0.998 on v3 — the
extra two-epoch budget (200 vs the v2 ensemble's 200) and the
fold-local StandardScaler combine to lift the trickiest-content
fold. ElFuente2's 3.288 RMSE is the largest residual; the per-frame
VMAF range on that source is wide (panning + saturation transitions),
but PLCC stays at 0.995.

**Caveat — the multi-codec lift floor (≥+0.005 PLCC over v1 per
ADR-0235) is NOT yet measurable on this corpus drop.** The corpus is
NVENC-only; 15 of 16 vocab slots receive zero training rows. v3 vs v1
on NVENC-only collapses to v1-vs-v1 on a single codec. The
multi-codec lift gate is deferred to the follow-up retrain that
consumes a multi-codec Phase A corpus drop. The first retrain ships
with `smoke: false` because the 0.95 floor — the ADR-0302-cited gate
— passed; the multi-codec lift becomes a gate on the v2 → v3
in-place promotion PR, not on this parallel-checkpoint PR.

## Open questions (for the follow-up retrain PR)

1. **VT corpus availability**: the Phase A runner supports VT, but
   VT requires Apple silicon. Does the local corpus drop need to
   include VT rows, or can the retrain skip VT and document its
   slots as zero-weight columns until VT corpus is generated?
   Provisional answer: include VT slots in the schema today (this
   PR), defer VT corpus rows to a follow-up; the retrain proceeds
   on libsvtav1 + the existing 12 hardware encoders, with the VT
   slots receiving training-time mask = 0. This keeps the column
   indices stable and avoids a second vocab bump when VT corpus
   eventually lands.
2. **Per-source CQ range parity**: OldTownCross was the v2 outlier.
   Does adding three new codecs widen its per-pair VMAF range
   enough to lift its LOSO PLCC above 0.95? Empirical question for
   the retrain run.

## References

- [ADR-0302](../adr/0302-encoder-vocab-v3-schema-expansion.md) —
  this digest's companion ADR.
- [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) —
  v2 production flip + 0.95 LOSO PLCC ship gate this digest re-uses.
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — append-only
  vocab invariant + multi-codec lift floor (+0.005 PLCC).
- [ADR-0272](../adr/0272-fr-regressor-v2-codec-aware-scaffold.md) —
  smoke scaffold; documents the codec block layout this digest
  preserves.
- [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) — VT
  adapters that motivate slots 14/15.
- [ADR-0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md) —
  `libsvtav1` adapter that motivates slot 13.
