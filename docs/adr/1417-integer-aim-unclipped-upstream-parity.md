<!-- markdownlint-disable MD013 MD060 -->
# ADR-1417: Integer AIM stays the unclipped ratio upstream defines; float AIM stays clipped at 1

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: adm, aim, numerics, netflix-compat, docs, fork-local

## Context

While the integer ADM masking threshold was being fixed
([ADR-1402](1402-adm-cm-centre-tap-int32.md)), a flat grey 64x64 reference
against the same picture with isolated patches scored `integer_aim`
3.1755853876204676 where `float_adm` reports an `aim` of exactly 1, before and
after that fix. AIM is documented as lying in [0, 1], so the question was
whether the integer extractor is wrong on such content
(`T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`).

AIM is the additive impairment that survives contrast masking, divided by the
DLM denominator, which measures the reference's own detail. The two pipelines
were compared stage by stage on that picture (scalar path, `--precision max`):

| Scale | Integer AIM numerator | Float AIM numerator | Denominator (both) |
|---|---|---|---|
| 0 | 12.072382 | 12.0720234 | 8.71317863 |
| 1 | 17.7911568 | 17.7916965 | 5.48895884 |
| 2 | 25.354557 | 25.3548603 | 3.77976322 |
| 3 | 9.44635677 | 9.44600391 | 2.38110161 |
| Sum | 64.66445255279541 | 64.664584159851074 | 20.363002300262451 |

The numerators agree to four significant digits (fixed point against float)
and the denominator is the same number: with a flat reference it is the noise
floor term alone. The ratios are 3.17558539 and 3.17559185. The only
difference is the last line of each extractor, and both lines are upstream's
(Netflix/vmaf `6ec23e8f2`):

```c
/* libvmaf/src/feature/integer_adm.c:3006-3007 */
// normalize AIM score by the DLM denominator
*score_aim = aim_num / den;

/* libvmaf/src/feature/adm.c:322-323 */
// normalize AIM score by the DLM denominator and clip values larger than 1
*score_aim = MIN(aim_num / aim_den, 1.0f);
```

Upstream master, built from that commit, prints `integer_aim` 3.175585 and
`aim` 1.000000 for the same two files. The fork mirrors both
(`vmaf_adm_scale_ratios()` for the integer extractor,
`vmaf_adm_finalize_scores()` for the float one, in
`core/src/feature/adm_score.h`).

The value reaches a model. `adm3` is
`max(w * adm2 + (1 - w) * (1 - aim), adm_min_val)`, and the shipped
`vmaf_v1.0.16` models read `VMAF_integer_feature_adm3_score` with `w = 0.7`
and `adm_min_val = 0.5`. Measured at 576x324 with the default model
`vmaf_v1.0.16_3d0h`, the fork against upstream master running the same model
file, and against a scratch build of the fork with the clip added:

| Flat grey reference against | Integer AIM (fork = upstream) | `adm3`, unclipped (fork = upstream) | `adm3` with a clip | VMAF, fork | VMAF, upstream | VMAF with a clip |
|---|---|---|---|---|---|---|
| isolated patches every 16 px | 2.623164 | 0.5 | 0.7 | 0 | 0.0 | 0 |
| uniform noise of +/-24 | 1.328354 | 0.601494 | 0.7 | 0 | 0.0 | 0 |
| uniform noise of +/-16 | 0.896739 | 0.730978 | 0.730978 | 5.766443 | 5.766443 | 5.766443 |
| uniform noise of +/-12 | 0.681863 | 0.795441 | 0.795441 | 24.880717 | 24.880717 | 24.880717 |

So the clip changes the model's ADM input once AIM passes 1, and on these
pictures the model's score has already reached 0 by then. No picture with an
integer AIM above 1 and a non-zero default-model score was found; none was
ruled out either.

## Decision

The integer extractor keeps the unclipped ratio and the float extractor keeps
its clip. The fork does not unify them. The difference is documented in
[`docs/metrics/features.md`](../metrics/features.md), recorded as an upstream
inconsistency in
[`docs/development/known-upstream-bugs.md`](../development/known-upstream-bugs.md),
and pinned by `test_integer_adm_aim_unclipped`, which fails if either
extractor changes sides.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clip integer AIM at 1, as the float extractor does | One documented range for both extractors; `adm3` could not fall below `w * adm2`; the default model's score did not move on any picture tried | Changes `VMAF_integer_feature_aim_score` and `VMAF_integer_feature_adm3_score` away from upstream libvmaf's on every frame with an integer AIM above 1 (`adm3` 0.5 to 0.7 on the patch picture), for the shipped `vmaf_v1.0.16` models and for anyone reading the features; no Netflix golden assertion covers the case, so nothing would flag a later drift; whether the models were fitted to the clipped or the unclipped feature is not known here | The integer extractor is what upstream ships and what the models consume; the fork follows it until upstream changes it |
| Remove the clip from the float extractor | Also one behaviour for both | Departs from upstream's float reference; float `aim` and `adm3` would change for every user of `float_adm` | Same reason, in the other direction |
| Leave the documentation saying [0, 1] for both | No change | The range is wrong for the integer extractor, and the next reader runs the same investigation | The documented range was the defect |
| Add an option that selects the clip | Lets a user choose | A new knob on a model feature that no model would set; a second way to compute a score upstream computes one way | No user asked for it |

## Consequences

- **Positive**:
  - The documented range of `aim_score` and the definition of `adm3_score` are
    correct for both extractors.
  - A test pins the integer value against upstream's printed 3.175585 and the
    float clip at 1, so a change to either is deliberate.
- **Negative**:
  - `adm` and `float_adm` keep disagreeing on content whose additive
    impairment exceeds the reference's own detail: flat or very smooth
    references with added noise, grain, dither or isolated artefacts. Their
    `adm3` disagrees there too.
- **Neutral / follow-ups**:
  - If upstream adds the clip to `integer_adm.c`, port it and re-run the
    golden gate; `test_integer_adm_aim_unclipped` then needs its integer
    expectations changed in the same PR.
  - The CUDA and SYCL twins of the integer extractor return the CPU's value on
    the patch picture (identical at `--precision max`); the HIP twin has no AIM
    pass; the Metal twin could not be run.
  - When the denominator is exactly 0, upstream leaves `score_aim`
    uninitialised in both extractors; the fork reports 1 for a 0 / 0 ratio and
    fails the frame for a non-zero numerator over 0 (unchanged here).

## References

- Task brief (rc3 worker assignment, 2026-10-01): "integer_aim is 3.18 on the
  64x64 flat-plus-patches picture [...] where float_adm's aim is 1, before and
  after #1700. Determine whether integer aim is wrong there [...] or whether
  the two legitimately differ on that content. [...] If not: document why in
  docs/metrics and close it with evidence."
- Netflix/vmaf `6ec23e8f2`: `libvmaf/src/feature/integer_adm.c:3007`,
  `libvmaf/src/feature/adm.c:323`; the clip entered `adm.c` with `4dcc2f7ce`
  (2026-04-27), ten days after `966be8d59` (2026-04-17) gave the integer
  extractor its AIM.
- [Research-1417](../research/1417-integer-aim-unclipped-upstream-parity.md):
  the stage numbers, the upstream build, the device twins.
- [ADR-1362](1362-sycl-integer-adm-aim-device-pass.md) (the AIM pass and its
  bit-exact contract), [ADR-0746](0746-cuda-integer-adm3-aim-parity.md),
  [ADR-1402](1402-adm-cm-centre-tap-int32.md),
  [ADR-0024](0024-netflix-golden-preserved.md).
- `docs/state.md`: `T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`.
