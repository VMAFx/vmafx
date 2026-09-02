# Research-0096 — Feature-extractor deduplication by provided-feature names

**Date:** 2026-05-10
**Status:** Accepted (fix landed in fix/fex-dedup-by-provided-feature)
**Bug ID:** T-CUDA-FEATURE-EXTRACTOR-DOUBLE-WRITE
**ADR:** [ADR-0385](../adr/0385-fex-dedup-by-provided-feature.md)

## Problem statement

When the CLI combines `--feature <name>` with an auto-loaded default VMAF
model (i.e. when `--no_prediction` is absent), two independent code paths
both call `feature_extractor_vector_append()`:

1. `vmaf_use_features_from_model()` — selects the active-backend extractor
   via `vmaf_get_feature_extractor_by_feature_name(name, fex_flags)`.
   With `--backend cuda` (or `HAVE_CUDA` compiled in and `gpumask == 0`)
   this resolves to the CUDA twin, e.g. `adm_cuda`.

2. The `--feature adm` CLI path — resolves the CPU extractor `adm` via
   `vmaf_get_feature_extractor_by_name("adm")` and appends it.

The old dedup key inside `feature_extractor_vector_append()` was:

```c
vmaf_feature_name_from_options(fex->name, fex->options, fex->priv)
```

This produces `"adm"` for the CPU extractor and `"adm_cuda"` for the
CUDA twin. The two strings differ, so the dedup check passes and both
extractors are registered. At every frame, both run their `extract()`
callback and both attempt to write the same `feature_collector` slot
(e.g. `VMAF_integer_feature_adm2_score` at index N). The second write
trips `vmaf_feature_collector_append()`'s overwrite guard, emitting:

```text
libvmaf WARNING feature "VMAF_integer_feature_adm2_score" cannot be overwritten at index N
```

For a 48-frame 576x324 run with `--feature adm`, the ADM extractor
emits 8 feature names × 48 frames = 384 such warnings. Across all model
features (adm, vif, motion) the total exceeds 750 per run.

## Root-cause analysis

The dedup function `vmaf_feature_name_from_options()` was designed to
produce a stable cache key for a single extractor — it incorporates the
extractor's `name` field (which differs between CPU and GPU twins) and any
non-default option values. It was never designed to detect inter-backend
twins, because when the function was written (upstream Netflix/vmaf) only
one backend existed. The GPU twins were added later by this fork without
updating the dedup logic.

The fundamental issue is that the function answers "is this the exact same
configured extractor?" rather than "would this extractor write to the same
feature slots?". The first question is the wrong one for dedup purposes.

## Fix

Change the dedup key from extractor-name to provided-feature names.
The `VmafFeatureExtractor.provided_features` field is a NULL-terminated
array of feature-name strings that the extractor emits. Per ADR-0214
(cross-backend parity contract), every CPU/GPU twin must emit the same
logical feature set. All real extractors in this codebase declare
`provided_features` (verified by audit).

If any entry in `provided_features[]` is shared between the already-
registered extractor and the incoming one, the incoming context is
destroyed and the registration is silently skipped. A DEBUG-level log
records which extractor was skipped and which registered extractor it
overlapped with.

A legacy fallback retains the original extractor-name comparison for
extractors with a NULL `provided_features` pointer, preserving backward
compatibility with any future extractor that omits the declaration.

## Alternatives considered

See [ADR-0385](../adr/0385-fex-dedup-by-provided-feature.md) §Alternatives
considered for the full decision matrix.

## Verification

**Before fix (reconstructed from PR #739 analysis):** `--feature adm` with
CUDA binary produced 750+ "cannot be overwritten" warnings per scoring run
(8 features × 48 frames × multiple extractors).

**After fix:** zero "cannot be overwritten" warnings on the same run:

```text
./build-cpu/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
  --feature adm --threads 1 2>&1 | grep "cannot be overwritten" | wc -l
# → 0
```

**Netflix golden score unchanged:** VMAF mean = 94.323010 on
`src01_hrc00_576x324.yuv` vs `src01_hrc01_576x324.yuv` (same as master).

**Unit test:** `test_fex_vector_dedup_by_provided_feature_name` in
`core/test/test_feature_extractor.c` exercises the new dedup path with
two synthetic extractors that share a provided-feature name, without
requiring CUDA to be compiled in.

## Scope of change

- `core/src/fex_ctx_vector.c` — new `provided_features_overlap()` helper
  updated `feature_extractor_vector_append()` dedup logic.
- `core/test/test_feature_extractor.c` — regression test.
- `core/test/meson.build` — adds `fex_ctx_vector.c` to the test target
  sources (was not linked in previously; the test exercised only the
  registry, not the vector).
- No public C-API change; no FFmpeg-patch impact; no model changes.
