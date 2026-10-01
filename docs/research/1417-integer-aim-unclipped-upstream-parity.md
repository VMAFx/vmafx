<!-- markdownlint-disable MD013 MD060 -->
# Research-1417: Integer AIM above 1 on a flat reference

- **Status**: Active
- **Workstream**: [ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md)
- **Last updated**: 2026-10-01

## Question

`integer_aim` is 3.18 on a flat 64x64 reference against the same picture with isolated patches, where `float_adm` reports an `aim` of 1. Is the integer extractor wrong on such content, or do the two extractors differ by definition?

## Sources

- `core/src/feature/integer_adm.c` (`integer_compute_adm()`, `adm_result_finalise()`), `core/src/feature/adm.c` (`compute_adm()`), `core/src/feature/adm_score.h` (`vmaf_adm_scale_ratios()`, `vmaf_adm_finalize_scores()`, `vmaf_adm3_score()`).
- Netflix/vmaf master `6ec23e8f2` (2026-09-30): `libvmaf/src/feature/integer_adm.c`, `libvmaf/src/feature/adm.c`, built with gcc 16.2.1 (`-Dc_args=-Wno-error=incompatible-pointer-types`; its `integer_vif.c` does not compile otherwise).
- Fork master `b22ad4e1a`, release build, gcc 16.2.1, Ryzen 9 9950X3D. The per-scale numbers come from a scratch copy of the tree with a `fprintf` after each scale; nothing of it is committed.
- Pictures: the 64x64 and 24x24 pictures of `core/test/test_integer_adm_cm_threshold.c` (flat grey 128; 4x2 patches `255 0 0 0`), the same patch grid at 576x324, and flat grey against uniform noise of +/-8, 12, 16 and 24 at 576x324.

## Findings

### The pipelines agree until the last line

AIM is the second contrast-masking pass with the two decoupled signals swapped: the additive impairment is measured and the restored signal masks it. Its numerator is summed over the four scales and divided by the DLM denominator. Scalar path, 64x64 patch picture:

| Scale | DLM numerator (both) | Denominator (both) | AIM numerator, integer | AIM numerator, float |
|---|---|---|---|---|
| 0 | 8.71317863 | 8.71317863 | 12.072382 | 12.0720234 |
| 1 | 5.48895884 | 5.48895884 | 17.7911568 | 17.7916965 |
| 2 | 3.77976322 | 3.77976322 | 25.354557 | 25.3548603 |
| 3 | 2.38110161 | 2.38110161 | 9.44635677 | 9.44600391 |
| Sum | 20.363002300262451 | 20.363002300262451 | 64.66445255279541 | 64.664584159851074 |

The reference is flat, so its detail bands are zero: the restored signal is zero, the whole distorted detail is additive impairment, and the DLM numerator and denominator are the noise-floor term alone (which is why `adm2` is exactly 1). The AIM numerators differ in the fifth digit, the usual distance between the fixed-point and the float pipeline. The ratios are 3.1755854 and 3.1755918. AVX2 and AVX-512 give the scalar's numbers bit for bit.

The 24x24 picture behaves the same: integer numerators 5.66737318, 8.09389496, 5.18849945, 6.01917458 against float 5.6671958, 8.09409428, 5.18855143, 6.01902771, over a denominator of 11.538572669029236; ratios 2.16395 and 2.16394.

### The last line differs, upstream

```c
/* Netflix/vmaf 6ec23e8f2, libvmaf/src/feature/integer_adm.c:3006-3007 */
// normalize AIM score by the DLM denominator
*score_aim = aim_num / den;

/* Netflix/vmaf 6ec23e8f2, libvmaf/src/feature/adm.c:322-323 */
// normalize AIM score by the DLM denominator and clip values larger than 1
*score_aim = MIN(aim_num / aim_den, 1.0f);
```

The integer extractor received its AIM in `966be8d59` (2026-04-17) without a clip; the float extractor received its AIM, with the clip, in `4dcc2f7ce` (2026-04-27). The fork mirrors both: `adm_result_finalise()` calls `vmaf_adm_scale_ratios()`, which divides, and `compute_adm()` calls `vmaf_adm_finalize_scores()`, which divides and clips.

### The fork returns upstream's numbers

| Picture | Fork `integer_aim` | Upstream master | Fork `aim` (float) | Upstream |
|---|---|---|---|---|
| 64x64 patches | 3.1755853876204676 | 3.175585 | 1 | 1.000000 |
| 576x324 patches | 2.602848185401339 | 2.602848 | 1 | 1.000000 |
| 576x324, noise +/-24 | 1.2616298263467636 | 1.261630 | 1 | 1.000000 |
| 24x24, one patch | 2.1639541459399108 | 2.075515 | 1 | 1.000000 |

The 24x24 row differs for a reason unrelated to the clip: upstream's scale-3 AIM numerator is 4.99870968 where the fork and the float extractor have 6.019. A 24x24 frame has a 2x2 scale-3 band, the size at which upstream's scale-3 DWT reads outside its input (`T-ADM-SCALE3-TINY-FRAME-OOB-READ-2026-09-18`, Netflix/vmaf PR #1599, fixed in the fork). At scales 0 to 2 the fork and upstream agree to float precision.

### Under the default model

`adm3 = max(w * adm2 + (1 - w) * (1 - aim), adm_min_val)`. The default model `vmaf_v1.0.16_3d0h` reads the integer `adm3` with `w = 0.7`, `adm_min_val = 0.5`, `adm_csf_mode = 2`, `adm_enhn_gain_limit = 1` and `adm_noise_weight = 0.02`. Fork against upstream master running the same model file, and against a scratch build of the fork with `MIN(aim, 1)` added, flat grey reference at 576x324:

| Distorted picture | Integer AIM | `adm3` (fork and upstream) | `adm3` with a clip | VMAF, fork | VMAF, upstream | VMAF with a clip |
|---|---|---|---|---|---|---|
| noise +/-8 | 0.461215 | 0.861636 | 0.861636 | 47.440577 | not run | 47.440577 |
| noise +/-12 | 0.681863 | 0.795441 | 0.795441 | 24.880717 | 24.880717 | 24.880717 |
| noise +/-16 | 0.896739 | 0.730978 | 0.730978 | 5.766443 | 5.766443 | 5.766443 |
| noise +/-24 | 1.328354 | 0.601494 | 0.7 | 0 | 0.0 | 0 |
| patches every 16 px | 2.623164 | 0.5 | 0.7 | 0 | 0.0 | 0 |

The fork and upstream agree in every row. A clip would change the model's ADM input in the last two rows; the model's score is 0 there with or without it. On this family of pictures the score reaches 0 before AIM reaches 1. A picture with an AIM above 1 and a score above 0 was not found, and the search was not exhaustive.

### Device twins

`adm_cuda` (RTX 4090) and `adm_sycl` (Arc A380) return the scalar CPU's output on the 64x64 patch picture, identical at `--precision max` for all seven ADM metrics, `integer_aim` included. `adm_hip` has no AIM pass and the request falls back to the CPU extractor. `integer_adm_metal` could not be run.

### The golden gate does not cover the case

`python/test/` asserts an integer AIM score in 48 places; the expected values run from 0.0 to 0.02656. None is above 1, so the gate would pass with or without a clip. `test_integer_adm_aim_unclipped` pins the 64x64 picture instead: integer AIM within 1e-6 of upstream's 3.175585, float AIM exactly 1, `adm3` 0 and 0.5 at the defaults and 0.5 and 0.7 with the model's weight and floor, and the scalar's bits on every dispatch level. With `MIN(aim, 1)` planted in `adm_result_finalise()` it fails with `integer_aim 1, upstream master 3.175585`.

## Alternatives explored

See the decision matrix of [ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md).

## Open questions

- Which of the two upstream lines is intended. The comment on the float one says the clip is deliberate there; nothing says its absence from the integer one is. The shipped models read the integer feature. This has not been raised upstream.
- When the denominator is exactly 0 upstream assigns `score` and leaves `score_aim` unset in both extractors. The fork reports an AIM of 1 for 0 / 0 and fails the frame for a non-zero numerator over 0.

## Related

- [ADR-1417](../adr/1417-integer-aim-unclipped-upstream-parity.md), [ADR-1362](../adr/1362-sycl-integer-adm-aim-device-pass.md), [ADR-0746](../adr/0746-cuda-integer-adm3-aim-parity.md), [ADR-1402](../adr/1402-adm-cm-centre-tap-int32.md).
- `docs/state.md`: `T-ADM-INTEGER-AIM-ABOVE-ONE-2026-10-01`.
