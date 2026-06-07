# AGENTS.md — core/src/sycl

Orientation for agents working on the SYCL / DPC++ backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Scope

The SYCL-side runtime (queue management, USM, dmabuf import). SYCL
**feature kernels** live in [../feature/sycl/](../feature/sycl/).

```text
sycl/
  common.cpp/.h             # queue creation, device selection, error-check
  picture_sycl.cpp/.h       # VmafPicture on a SYCL device (USM-backed)
  dmabuf_import.cpp/.h      # Linux DMA-BUF import path for zero-copy Level Zero
```

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **Compiler is `icpx` (Intel oneAPI) or AdaptiveCpp `acpp` / `syclcc`
  (ADR-0335).** `clang++ -fsycl` is also accepted in spirit but is not
  CI-tested. Do not assume MSVC-style extensions.
- **Intel-specific kernel attributes go through
  `core/src/feature/sycl/sycl_compat.h`** (ADR-0335). The
  `VMAF_SYCL_REQD_SG_SIZE(N)` macro expands to
  `[[intel::reqd_sub_group_size(N)]]` under icpx and to a no-op under
  AdaptiveCpp. New kernel sites that need an Intel-specific attribute
  add a new macro to `sycl_compat.h` rather than hard-coding the
  attribute. **On rebase**: if an upstream cherry-pick brings in a
  bare `[[intel::*]]` attribute on a SYCL kernel lambda, wrap it in a
  compat macro before merging.
- **Experimental flags enabled**: `-fsycl-unnamed-lambda`,
  `-fsycl-allow-func-ptr`, `-fsycl-device-code-split=per_kernel`. See
  [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md).
- **USM allocations**: prefer device USM for kernel-resident data, shared
  USM for cross-side communication. Host USM only when an adjacent API
  requires a pointer the host can dereference directly.
- **dmabuf import** is Linux-only and gated at build time; no callers
  should assume the FD path exists on other OSes.
- **Numerical snapshots**: same rule as CUDA — see CLAUDE.md §9.
- **`-fp-model=precise` is load-bearing.** The SYCL feature build
  line in `core/src/meson.build` adds `-fp-model=precise` to
  every per-kernel TU. This blocks `icpx` from FMA contraction in
  the kernel lambdas and matches the GLSL `precise` /
  `NoContraction` decorations on the Vulkan twins. Removing it
  drifts `float_adm_sycl` ([ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md))
  past `places=4` at scale 2 and `ssimulacra2_sycl`
  ([ADR-0206](../../../docs/adr/0206-ssimulacra2-cuda-sycl.md))
  past `places=2` through the IIR. **On rebase**: keep this flag
  on the SYCL feature line.
- **fp64-free kernels are load-bearing
  ([ADR-0220](../../../docs/adr/0220-sycl-fp64-fallback.md), T7-17).**
  Every SYCL feature-kernel lambda must capture and operate on
  `float` / integer types only. No `double` operand inside a
  `parallel_for` body, no `sycl::reduction<double>`, no
  `sycl::plus<double>`. This is hard, not soft: a single fp64
  instruction anywhere in the TU's SPIR-V module causes the
  Level Zero runtime to reject the entire module on Intel Arc
  A-series and other fp64-less devices, even when the offending
  kernel is never submitted. `double` is allowed *outside* the
  kernel lambda (host-side post-processing in `extract` /
  `flush` callbacks, score aggregation, log10 normalisation).
  ADM gain limiting uses int64 Q31 (`gain_limit_to_q31` +
  `launch_decouple_csf<false>` in `integer_adm_sycl.cpp`); VIF
  gain limiting uses fp32 `sycl::fmin`. **On rebase**: if an
  upstream cherry-pick brings a `double` into a kernel lambda,
  refactor it to int64 / fp32 before merging.
- **VAAPI / dmabuf zero-copy import surface
  ([ADR-0183](../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md))**:
  `vmaf_sycl_import_va_surface` is consumed by the `libvmaf_sycl`
  FFmpeg filter (`ffmpeg-patches/0005-*.patch`). Symmetric to the
  Vulkan VkImage import in
  [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md).
  Public surface change touches the patch file too — see
  CLAUDE.md §12 r14.

## Rebase-sensitive build invariants

- **`-fsycl` must be present at every link step that pulls SYCL `.o`
  files from a static archive.** This is enforced by embedding `-fsycl`
  in `sycl_dependency.link_args` in `core/src/meson.build` (ADR-1099).
  Without `-fsycl` at link time, `icpx` skips `clang-offload-wrapper`,
  the SYCL runtime's `ProgramManager` never registers device kernels,
  and the first `queue.submit()` crashes with a null-dereference inside
  `getDeviceKernelInfo`. **On rebase**: if a branch splits `sycl_dependency`
  into compile-time and link-time halves, or introduces a new `declare_dependency`
  block for a SYCL sub-target, ensure `-fsycl` remains in the link-time
  `link_args` of every dependency that SYCL consumers inherit.

- **Feature-name aliasing when querying scores from non-default SYCL extractors.**
  When any `VMAF_OPT_FLAG_FEATURE_PARAM` option is set to a non-default
  value, `vmaf_feature_name_from_opts_dict` stores scores under an aliased
  name (not the raw `VMAF_*_score` name). The aliased name is:
  `<alias_of_raw_name>_<opt1_alias>_<opt2_alias>...` in alphabetical option
  order. Example: `motion_sycl` with `motion_add_uv=true` stores
  `VMAF_integer_feature_motion2_score` as `integer_motion2_mau`. Test code
  querying `vmaf_feature_score_at_index` must use the aliased name, not
  the raw name. (ADR-1099)

## Rebase-sensitive invariants per kernel

- **`feature/sycl/integer_psnr_sycl.cpp` chroma planes ride on per-
  extractor device buffers, NOT the shared frame buffer.** The
  `vmaf_sycl_shared_frame_init` pipeline is luma-only by design (see
  the `shared_*` documentation in `common.h`). Chroma upload happens
  in the combined-graph `pre_fn` callback as a host-staged H2D copy
  into `PsnrStateSycl::d_chroma_{ref,dis}[]`; the chroma SSE kernel
  fires from `post_fn` (direct, post-graph) on the same in-order
  combined queue. **On rebase**: if an upstream sync extends
  `vmaf_sycl_shared_frame_init` to allocate chroma planes, the PSNR
  extension can be migrated onto it and the per-extractor chroma
  buffers retired — but only after a cross-backend gate run confirms
  bit-exactness against CPU at `places=4` (see ADR-0214). T3-15(b),
  ADR-0192 §"Status update 2026-05-09: T3-15 #2 SYCL PSNR chroma".

## Governing ADRs

- [ADR-0002](../../../docs/adr/0002-merge-path-master-default.md) —
  sycl branch → master merge history.
- [ADR-0016](../../../docs/adr/0016-sycl-to-master-merge-conflict-policy.md)
  — merge-conflict policy.
- [ADR-0022](../../../docs/adr/0022-inference-runtime-onnx.md) —
  OpenVINO EP mapping for SYCL.
- [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md) —
  experimental SYCL flags.
- [ADR-0220](../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels are unconditionally fp64-free (T7-17).
- [ADR-0335](../../../docs/adr/0335-adaptivecpp-second-sycl-toolchain.md)
  — AdaptiveCpp added as a second SYCL toolchain alongside icpx;
  Intel-specific kernel attributes routed through
  `feature/sycl/sycl_compat.h`.

## Build

```bash
meson setup build -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.

Alternative: AdaptiveCpp (open-source, ADR-0335).

```bash
meson setup build-acpp -Denable_cuda=false -Denable_sycl=true \
    -Dsycl_compiler=acpp -Dsycl_acpp_targets=generic
ninja -C build-acpp
```

See
[`docs/development/sycl-toolchains.md`](../../../docs/development/sycl-toolchains.md)
for the per-toolchain capability matrix and numerical conformance
notes.
