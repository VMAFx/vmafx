<!-- markdownlint-disable MD013 -->

# 2033 — Options whose default is an arithmetic identity are untested by default-options parity tests

**Date**: 2026-09-07
**Scope**: `motion_fps_weight` on the v1 `integer_motion` GPU twins (CUDA, SYCL,
HIP); the general class of `VMAF_OPT_FLAG_FEATURE_PARAM` options whose default
value is a no-op for the arithmetic they control.
**Outcome**: three real cross-backend defects fixed
([ADR-1216](../adr/1216-gpu-motion3-fps-weight-applied-once.md)); the parity
tests for the affected feature gained non-default-option variants.

## The defect

The CPU reference applies `motion_fps_weight` in exactly one place —
`extract()` in `core/src/feature/integer_motion.c`:

```c
score = MIN((double)sad / 256. / (w * h) * s->motion_fps_weight, s->motion_max_val);
```

That weighted value is what lands in the feature collector as
`motion_sad_score`. `flush()` reads it back, derives `motion2` as a min over
neighbouring entries, and blends `motion2` into `motion3`:

```c
double processed = MIN(motion_blend(motion2, s->motion_blend_factor,
                                    s->motion_blend_offset), s->motion_max_val);
```

No second weighting. Each of the three GPU twins reproduced this blend in a
host-side helper that opened with the weight instead:

```c
double const weighted = score2 * s->motion_fps_weight;   /* CUDA, SYCL, HIP */
double const blended  = motion_blend(weighted, s->motion_blend_factor,
                                     s->motion_blend_offset);
```

and every caller in all three twins already passed a weighted, clipped value:

```c
double const last_motion2 = MIN(s->score * s->motion_fps_weight, s->motion_max_val);
double const motion3_score = motion3_postprocess_cuda(s, last_motion2);
```

So `motion3_score` carried `motion_fps_weight²`. Measured on the RTX 4090 with a
256x144 8-bpc fixture at `motion_fps_weight = 0.6`: `cpu = 14.48987751`,
`gpu = 8.69392654` — exactly `14.48987751 × 0.6`, a `5.8` absolute drift against
the `1e-4` ADR-0214 gate. The SYCL (Arc A380) and HIP (gfx1030) twins reproduce
the same three-significant-figure-identical drift, because the defect is in
host-side scalar code shared in shape across the three.

## Why every gate was green

`motion_fps_weight` defaults to `1.0`, and `1.0² = 1.0`.

Every motion3 parity test in the tree instantiated its extractors with `NULL`
options:

```c
err = vmaf_use_feature(vmaf, "motion", NULL);
err = vmaf_use_feature(vmaf, "motion_cuda", NULL);
```

Under the default the two paths compute the same number, so CPU and GPU agreed
— on a value neither was computing correctly for any other weight. The test was
not weak in tolerance, coverage of frames, or fixture size; it simply never
moved the one parameter that separates the two implementations.

## The generalisable rule

**An option is covered by a parity test only if the test sets it away from a
value that makes the code paths coincide.** For a multiplicative correction that
means any value except `1.0`; for an additive one, any value except `0`; for a
clip, any value the data actually reaches; for a boolean, both settings.

This is the third instance of the same shape in the fork's GPU twins:

| Digest / ADR | Pinned parameter that hid the defect |
| --- | --- |
| [2032](2032-gpu-parity-resolution-blind-spot.md) / [ADR-1206](../adr/1206-gpu-parity-large-fixture-variants.md) | Every fixture was 256x144, so band dimensions never exceeded the ADM border-crop threshold |
| [ADR-1204](../adr/1204-adm-cm-edge-clamp-gpu-twins.md) | Same fixture size — the asymmetric ADM edge rule only diverges when the crop is 0 |
| This digest / [ADR-1216](../adr/1216-gpu-motion3-fps-weight-applied-once.md) | Every option left at its default, and this default is an arithmetic identity |

The first two were about *fixture* shape. This one is about *option* values,
which is the cheaper axis to sweep: it needs no new fixture, only a second
`vmaf_use_feature()` call with a dictionary.

## How the derived key is formed

Setting a `VMAF_OPT_FLAG_FEATURE_PARAM` option changes the key the score is
filed under ([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)):
the alias base plus `_<alias>_<value>` per non-default param, sorted by option
*name*, values formatted with `%g`. With only `motion_fps_weight = 0.6` set,
`VMAF_integer_feature_motion3_score` is read back as
`integer_motion3_mfw_0.6`. A parity variant that forgets this reads the
undecorated key, gets `score_at_index failed`, and looks like a harness bug
rather than a coverage win.

## Candidates worth the same treatment

Identity-defaulted `FEATURE_PARAM` options on features that have GPU twins,
found by grepping the option tables for `default_val.d = 1.0` /
`default_val.d = 0.0` / `default_val.b = false`:

- `motion_blend_factor` (`1.0` — `motion_blend()` degenerates to the identity)
- `adm_enhn_gain_limit`, `vif_enhn_gain_limit` (`1.0` — the gain clamp is a
  no-op at unity)
- `adm_csf_scale`-family scalars where `1.0` cancels the divisor
- every `VMAF_OPT_TYPE_BOOL` param, which is only half-covered by definition

Each is a candidate for the same one-extra-`vmaf_use_feature()`-call variant.
