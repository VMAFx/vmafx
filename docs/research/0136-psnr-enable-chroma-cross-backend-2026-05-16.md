# Research-0136: PSNR `enable_chroma` cross-backend silent divergence — 2026-05-16

**Date**: 2026-05-16
**Author**: Claude (Anthropic) on behalf of Lusoris
**Scope**: `core/src/feature/cuda/integer_psnr_cuda.c`,
           `core/src/feature/sycl/integer_psnr_sycl.cpp`,
           `core/src/feature/vulkan/psnr_vulkan.c`
**Status**: Resolved by ADR-0452 (PR to follow)

## Finding

The feature-option parity audit of 2026-05-16 identified that the three GPU
PSNR extractors (`psnr_cuda`, `psnr_sycl`, `psnr_vulkan`) each had an empty
`VmafOption options[] = {{0}}` table — they exposed no user-tunable options
despite the CPU reference extractor (`integer_psnr.c`) exposing five:
`enable_chroma`, `enable_mse`, `enable_apsnr`, `reduced_hbd_peak`, and
`min_sse`.

The most impactful missing option is `enable_chroma` because it affects which
output metrics are emitted. The CPU default is `true` (emit `psnr_y`,
`psnr_cb`, `psnr_cr`). The GPU extractors hardcoded the equivalent of
`enable_chroma=true` in their `init()` geometry setup (all three always set
`n_planes = PSNR_NUM_PLANES = 3` for non-YUV400 sources), but:

- A caller passing `enable_chroma=false` via `vmaf_use_feature()` options
  would have the option silently rejected (unknown-option path) and the GPU
  extractor would continue emitting full chroma, diverging from the CPU
  reference which would honour the flag and emit only `psnr_y`.
- A model that depends on luma-only PSNR could produce different JSON output
  depending on which backend ran — without any error or warning.

## Root cause

The GPU twin files were scaffolded with a stub `options[] = {{0}}` that was
never filled in from the CPU reference. The `enable_chroma` field was never
added to the state struct, so no `offsetof()` target existed. The compute
kernels themselves are correct; only the option-dispatch path was missing.

## Fix shape

For each of the three GPU twins:

1. Add `bool enable_chroma` to the per-extractor state struct (default
   initialised to `true` by the options framework).
2. Add an option entry to `options[]` matching the CPU table entry verbatim
   (same `name`, `help`, `offset`, `type`, `default_val.b = true`).
3. In `init()`, after the existing `pix_fmt == VMAF_PIX_FMT_YUV400P` branch
   that clamps `n_planes` to 1, add a second guard:

   ```c
   if (!s->enable_chroma && s->n_planes > 1U) {
       s->n_planes = 1U;
       s->width[1] = s->width[2] = 0U;
       s->height[1] = s->height[2] = 0U;
   }
   ```

   This mirrors `integer_psnr.c::init`'s `if (pix_fmt == VMAF_PIX_FMT_YUV400P)
   s->enable_chroma = false` pattern, adapted for the GPU side where `n_planes`
   controls the dispatch loop rather than a separate flag.

4. All downstream loops (`submit`, `collect`, `close`) already iterate
   `for (p = 0; p < s->n_planes; p++)` so they require no changes — the
   geometry clamp in `init()` is sufficient.

## Bit-exactness impact

Zero at default (`enable_chroma=true`). The option path is new code; when
the default is in effect, `n_planes` equals the same value as before the
change on every non-YUV400 source. The compute kernels, readback layout, and
score emission logic are untouched.

## Parity gate command

```bash
# Default (enable_chroma=true) — must gate at places=4 vs CPU:
python3 scripts/ci/cross_backend_parity_gate.py --backends cpu cuda --features psnr --places 4

# enable_chroma=false — GPU must match CPU luma-only output:
python3 scripts/ci/cross_backend_parity_gate.py \
    --backends cpu cuda \
    --features psnr \
    --places 4 \
    --feature-opts 'psnr=enable_chroma=false' \
    --feature-opts 'psnr_cuda=enable_chroma=false'
```

## References

- ADR-0452 (accompanying decision record)
- CPU reference: `core/src/feature/integer_psnr.c::init` lines 123-125
- PR #880 (motion_fps_weight option-parity pattern)
- Audit plan: `.workingdir/feature-option-parity-audit-2026-05-16.md` (P1 item)
