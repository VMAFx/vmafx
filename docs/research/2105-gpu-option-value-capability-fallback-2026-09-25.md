<!-- markdownlint-disable MD013 MD060 -->
# Research-2105: GPU option-value capability fallback — 2026-09-25

**Status:** Complete

**Authority inspected:** signed collector
`d5df7ec9501a2781984b20835dccabf12e77bbf8`

**Scope:** model-driven feature-extractor selection, internal option metadata,
and every GPU init-time option restriction on the collector. No kernel
arithmetic, model, snapshot, Netflix golden assertion, CLI syntax, public
header, or FFmpeg integration change.

## Failure path

ADR-1183 checks whether a selected GPU extractor declares every key in the
model's option dictionary. It does not ask whether the implementation can
execute the supplied value. The context parser then accepts any value inside
the declared range, and the selected extractor's `init()` rejects its narrower
implementation range with `-EINVAL`. CPU fallback is never attempted because
the key itself exists.

Narrowing the GPU table would not be an honest repair. Feature names are
derived from the selected extractor's option metadata; the CPU name, alias,
default, range and `FEATURE_PARAM` identity remain the schema authority even
when one backend implements a subset.

## Default-only, CPU-capable inventory

| Family | Backends | Declared option | GPU values | CPU capability |
|---|---|---|---|---|
| `float_vif` | CUDA, SYCL, HIP, Metal | `vif_kernelscale`, `0.1..4.0` | `1.0` only | Full declared range |
| `float_adm` | CUDA, SYCL, HIP, Metal | `adm_csf_mode`, CPU-compatible range | `0` only | All declared modes |
| `integer_adm` | Metal | `adm_csf_mode`, `0..3` | `0` only | Modes `0..3`, subject to its documented representability guard |

These are the nine sites marked `VMAF_OPT_FLAG_DEFAULT_ONLY`. CUDA
`float_vif` omits `vif_skip_scale0`; that name absence already takes
ADR-1183's safe CPU fallback and is not part of this defect.

## Complete init-restriction audit and exclusions

The audit paired every option-dependent GPU init rejection with the CPU
extractor selected for the same feature identity. It found three other shapes:

| Restriction | Backends | Why `DEFAULT_ONLY` is wrong | Disposition |
|---|---|---|---|
| `integer_motion.motion_five_frame_window=true` | CUDA, SYCL, HIP | CPU `integer_motion` and `motion_v2` also reject the value under ADR-0337; fallback cannot complete | Existing five-frame plumbing backlog remains authoritative |
| `integer_motion.motion_add_uv=true` | CUDA, HIP, Metal | The CPU `integer_motion` twin has no such option; SYCL implements it and CPU `float_motion` is a different extractor/feature identity | Existing ADR-0989 backend-port backlog remains authoritative |
| `float_ssim.scale` resolves above `1` | CUDA, SYCL, HIP, Metal | Explicit `scale=1` is supported even though the declared default is `0`; default `0` is auto and becomes unsupported only from frame dimensions | Recorded separately as `T-GPU-FLOAT-SSIM-AUTO-SCALE-CAPABILITY-2026-09-25`; it needs context-aware dispatch or a real decimation port |

Geometry, pixel-format, bit-depth, missing-device and allocation failures were
also inspected. They are input/runtime capability failures rather than option
values with a more capable CPU twin, so they do not belong in option metadata.

## Red cap and implementation seam

`core/test/test_gpu_option_value_capability_contract.py` enumerates all nine
default-only entries. Before implementation it failed all nine subtests. The C
unit test covers boolean, integer, double, aliases, minimum and maximum valid
boundaries, an unknown option, malformed/out-of-range values, and an
unrestricted non-default option.

The helper parses a supplied value with `vmaf_option_set()` into aligned typed
storage. Valid non-default values report the caller's exact key and require
fallback. Invalid values deliberately remain the ordinary context parser's
responsibility, so invalid input is not disguised as a backend fallback.

The collector white-box test crosses the production selection seam. It proves
that valid `vif_kernelscale` minimum and maximum values select the real CPU
`float_vif` provider, the default preserves the selected mock GPU twin, and a
malformed value stays on that twin for normal parser rejection.

## Verification

- RED: 9/9 source-contract subtests failed before metadata was added.
- GREEN: the source inventory and both C unit executables pass in a fresh GCC
  15 CPU build inside an isolated container.
- The isolated CPU fast suite passes 158/158. A separate `icx`/`icpx`
  all-backend configuration compiles and links all 402 CUDA, SYCL and HIP
  targets; Metal remains covered by the device-free inventory contract because
  the Linux verifier has no Apple SDK.
- Independent correctness review found that the structured context-create
  cleanup preserved a pre-existing private-state leak when an optionless
  extractor rejected a nonempty option dictionary. The added NIQE case makes
  LeakSanitizer fail on the reviewed pre-repair head with a 264-byte direct
  leak and pass after the cleanup helper releases the owned private state.
- Exact clang-tidy 22.1.8 / GCC 16.2.1 touched-file measurements are at or
  below every CPU, CUDA, HIP and SYCL allowance, with zero compile failures and
  zero uncited suppressions. Generated scoped writes tighten the touched CPU
  test allowance from 16 to 12 and both CUDA allowances from 5 to 3.
- The Praetor audit reports all 39 touched files HISS-clean, HISS evidence
  replay passes 18/18, staged pre-commit passes, and MkDocs strict validation
  passes. Final signed-head AGY review receipts belong in the handoff because
  recording them here would change the head they reviewed.

## Result

Automatic model dispatch now distinguishes schema compatibility from a
default-only backend implementation. Valid non-default requests fall back per
feature to CPU, while valid defaults remain on device, malformed values still
fail normally, and explicit GPU-extractor requests retain their direct error.
