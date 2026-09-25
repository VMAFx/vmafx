<!-- markdownlint-disable MD013 MD060 -->
# ADR-1322: Restore `enable_chroma` option parity on `integer_psnr_metal`

- **Status**: Accepted
- **Date**: 2026-09-25
- **Deciders**: Kilian, Lusoris
- **Tags**: metal, psnr, option-parity, chroma, fork-local, bug-048

## Context

ADR-0453 (CUDA/SYCL) and ADR-0471 (HIP) established GPU option parity for integer PSNR:
all GPU twins must implement `enable_chroma` (default `true`, false forces luma-only) and
`uncapped` (default `false`). When `enable_chroma=false` or when `pix_fmt == VMAF_PIX_FMT_YUV400P`,
the extractor clamps active planes to 1 and suppresses chroma execution and feature collection.

While PR #986 originally added this contract to Metal (`integer_psnr_metal.mm`), PR #1067
(bootstrap name-builder refactor) merged a stale branch base that accidentally clobbered
`enable_chroma` in `integer_psnr_metal.mm` (recorded in `docs/rebase-notes.md:44980`). As a result:

1. `integer_psnr_metal` rejected `--feature integer_psnr_metal=enable_chroma=false` with `-EINVAL`.
2. For monochrome sources (`YUV400P`) or callers disabling chroma, `integer_psnr_metal`
   unconditionally looped over 3 planes in `submit()` and `collect()`, reading unallocated plane
   memory and emitting spurious `psnr_cb` and `psnr_cr` sub-scores.
3. Metal diverged from CUDA, SYCL, and HIP GPU twins.

## Decision

Restore `enable_chroma` to `integer_psnr_metal.mm` with full parity to CUDA, SYCL, and HIP:

1. Add `bool enable_chroma` and `unsigned n_planes` to `IntegerPsnrStateMetal`.
2. Register `enable_chroma` in `options[]` as `VMAF_OPT_TYPE_BOOL` defaulting to `true`.
3. In `init_fex_metal()`, compute `n_planes`: clamp to 1 for `VMAF_PIX_FMT_YUV400P` or `!enable_chroma`,
   and allocate partial readback buffers only for active planes.
4. In `submit_fex_metal()` and `collect_fex_metal()`, loop over `s->n_planes` rather than a fixed 3-plane constant.
5. Guard the contract with device-free regression test `test_gpu_psnr_option_parity_contract.py`
   running on all platforms in `meson test --suite=fast`, plus unit checks in `test_metal_kernel_registration.c`
   and `test_metal_integer_psnr_parity.c`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Restore `enable_chroma` to `integer_psnr_metal.mm` with device-free contract test | Restores parity across all four GPU backends; closes BUG-048 correctness defect; prevents crash on YUV400P | Small code change in Metal host dispatch | **Chosen**: matches ADR-0453 and ADR-0471 architecture. |
| Leave Metal as luma+chroma only | No changes to Metal source | Fails parity; crashes or corrupts memory on YUV400P inputs; caller cannot disable chroma | Unacceptable correctness and safety defect. |

## Consequences

- **Positive**: `integer_psnr_metal` now achieves 100% option and plane-clamping parity with CUDA, SYCL, and HIP.
- **Positive**: YUV400P inputs no longer access out-of-bounds plane buffers in Metal PSNR.
- **Positive**: `test_gpu_psnr_option_parity_contract` guarantees future refactors cannot silently drop PSNR options on any GPU backend.
- **Neutral**: Default score outputs on YUV420P/422P/444P are bit-identical (default `enable_chroma=true` is preserved).
