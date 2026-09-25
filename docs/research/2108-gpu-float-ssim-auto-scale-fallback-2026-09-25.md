<!-- markdownlint-disable MD013 MD060 -->
# Research-2108: GPU float-SSIM auto-scale fallback — 2026-09-25

**Status:** Complete

**Authority inspected:** signed collector
`1e6f552ffe1c9f18911499f9b8c2ad4f3fd9f717`

**Scope:** dimension-dependent `float_ssim.scale` capability in the CUDA,
SYCL, HIP and Metal twins; model-selected lifecycle and CPU fallback. No score
arithmetic, kernel, model, snapshot, Netflix golden assertion, public C API,
CLI syntax, FFmpeg patch, benchmark, tuning or retraining change.

## Reproduction

The CPU and all four GPU twins calculate automatic scale from the short side:

```text
scale = max(1, round(min(width, height) / 256))
```

The GPU initializers then reject every result except `1`. The boundary is
therefore exact and deterministic:

| Dimensions | Short side | Resolved `scale=0` | Pre-fix GPU result |
|---|---:|---:|---|
| 320x240 | 240 | 1 | succeeds |
| 383x383 | 383 | 1 | succeeds |
| 384x384 | 384 | 2 | `-EINVAL` |
| 960x540 | 540 | 2 | `-EINVAL` |
| 1920x1080 | 1080 | 4 | `-EINVAL` |
| 960x540 with `scale=1` | 540 | 1 | succeeds |

This is not a malformed option. `0` is the canonical automatic value and the
CPU extractor implements the resolved decimation. It is also not expressible
through ADR-1316's `VMAF_OPT_FLAG_DEFAULT_ONLY`, because the same default value
is executable below the threshold and unsupported above it.

## Inventory and lifecycle seam

The live restriction is identical in:

- `core/src/feature/cuda/integer_ssim_cuda.c`;
- `core/src/feature/sycl/integer_ssim_sycl.cpp`;
- `core/src/feature/hip/float_ssim_hip.c`;
- `core/src/feature/metal/float_ssim_metal.mm`.

At model registration, options have been parsed but dimensions are not always
known. At backend `init()`, dimensions are known but picture residency has
already been chosen. The first host-picture read supplies the safe seam:
`read_pictures_validate_and_prep()` has established format, bit depth and
dimensions, while CUDA translation and every extractor initialization still
lie ahead.

An optional extractor `context_check` callback now runs at that seam only for
model-selected contexts. `0` preserves the selected extractor; `-ENOTSUP`
clones the already-validated option dictionary into the declared CPU fallback.
The old context is still uninitialized, so replacement cannot abandon device
allocations or pending work. Cached CUDA host/device requirements are marked
dirty before translation. A fallback error consumes the input pictures through
the ordinary pre-dispatch cleanup path.

Direct `vmaf_use_feature()` contexts never become fallback-eligible. Their
existing GPU `init()` errors remain authoritative, including explicitly named
`float_ssim_{cuda,sycl,hip,metal}` extractors. Malformed and out-of-range
values still fail while the original context parses its options, before the
dimension callback can run.

The device-buffer-only `vmaf_read_pictures_sycl()` entry point deliberately
does not use this host fallback: it has no host pictures for a CPU extractor.
Its scale-1-only contract remains explicit until a verified SYCL decimation
implementation exists.

## Red cap

`core/test/test_gpu_float_ssim_auto_scale_contract.py` was added before the
implementation. It failed five contract groups on the collector because the
framework callback, model-only eligibility bit, pre-translation resolver and
four backend declarations did not exist.

The white-box `test_feature_collector` regression executes the production
replacement helper with a synthetic GPU descriptor and the real CPU
`float_ssim` provider. It covers 320x240, 383x383, 384x384 and 960x540; proves
that explicit `scale=1` remains on the selected twin; proves that a direct
context is not replaced; and verifies that the option dictionary survives the
replacement.

## Delegated analysis evidence

AGY conversation `3ed62f73-c4ea-4056-95bf-20c51edab791` traced the option,
dispatch and first-frame lifecycle before provider quota stopped every writable
route without changing the worktree. Gemini returned
`RESOURCE_EXHAUSTED (code 429)` with `Resets in 1h33m52s`; Claude Sonnet
returned the same hard quota with `Resets in 4h49m36s`; the GPT-OSS route
shared that pool and reported `Resets in 4h48m5s`. Local implementation began
only after those routes were exhausted and the preserved analysis had been
reported to the supervising agent.

## Verification

- RED: the device-free contract failed five groups before the callback and
  declarations existed.
- GREEN: `test_gpu_float_ssim_auto_scale_contract`,
  `test_gpu_option_value_capability_contract` and the white-box
  `test_feature_collector` pass in a fresh CPU build.
- A complete CPU `ninja` build compiles all 1,606 targets. The CPU fast suite
  passes 161 tests, skips the device-serialization probe, and has zero
  failures across 162 cases; the complete CPU Meson suite passes 176, skips
  that same probe, and has zero failures across 177 cases.
- Complete configured builds pass with CUDA 13.4, the ROCm HIP runtime, and
  Intel oneAPI 2026.0 in SPIR-V JIT mode.
- The Netflix CPU golden-data gate passes 271 tests with 12 expected skips;
  no golden assertion was edited.
- Metal remains covered by the shared source contract on this Linux verifier;
  there is no Apple SDK or Metal device on this host.
- Exact clang-tidy 22.1.8 touched-file measurements have zero compile
  failures and zero uncited suppressions. Generated scoped writes tighten
  `integer_ssim_cuda.c` from 3 to 2 warnings and the touched collector test
  from 16 to 12 in the CUDA, HIP and SYCL lanes; no allowance increases.
  The full-lane ratchets still expose unrelated baseline drift tracked by
  `T-TIDY-RATCHET-GPU-LANES-UNREPRODUCIBLE-2026-09-22`.
- Formatting, Markdown lint, fragment regeneration, state-row validation,
  ADR numbering/link checks and source-to-ADR citation governance pass.
- No file below `python/test/` and no Netflix golden assertion changed.

## Result

Automatic host-picture model dispatch now evaluates the real picture context
before GPU initialization. Auto-scale `1` stays on the selected GPU twin;
auto-scale above `1` runs CPU `float_ssim` with the same validated options;
explicit extractor requests and parser failures remain unchanged.
