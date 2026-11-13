---
paths:
  - core/src/meson.build
  - core/src/sycl/scratch_check.cpp
  - core/src/sycl/dmabuf_import.cpp
  - core/src/feature/sycl/*.cpp
invariant: sycl_strict_fp_args compile/link flags, fp64-free parallel_for lambdas, zero-scratch audit, and VA surface ABI.
---
<!-- markdownlint-disable MD013 -->
# SYCL strict-FP flags, fp64-free lambdas, and zero-scratch

## Ground rules

- **SYCL strict FP line load-bearing
  ([ADR-1367](../../../../docs/adr/1367-sycl-strict-fp-every-feature-tu.md)).**
  `sycl_strict_fp_args` in `core/src/meson.build` (between
  `VMAF SYCL strict FP policy` markers) goes to every feature TU:
  `-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div
  -foffload-fp32-prec-sqrt`, order fixed. precise alone: `a * b + c` still
  one FMA in kernel lambdas, fp32 `/` and `sqrt` approximate (ADR-1358);
  no precise: `float_adm_sycl` past `places=4`
  ([ADR-0202](../../../../docs/adr/0202-float-adm-cuda-sycl.md)). Precision
  pair also on `sycl_dependency` link (not Windows): SPIR-V JIT image made
  there. **On rebase**: keep one line for all TUs, no per-TU FP list, keep
  link pair; `test_strict_fp_compiler_args.py` +
  `test_sycl_fp_arith_contract` fail otherwise.
- **fp64-free kernels load-bearing
  ([ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md), T7-17).**
  Every SYCL feature-kernel lambda must capture and operate on
  `float` / integer types only. No `double` operand inside
  `parallel_for` body, no `sycl::reduction<double>`, no
  `sycl::plus<double>`. Hard, not soft. Single fp64 instruction
  anywhere in TU's SPIR-V module -> Level Zero runtime rejects entire
  module on Intel Arc-series and other fp64-less devices. Applies
  even when offending kernel never submitted. `double` allowed
  *outside* kernel lambda (host-side post-processing in `extract` /
  `flush` callbacks, score aggregation, log10 normalisation). ADM gain
  limiting uses integer form of CPU's truncated double product
  (`adm_gain_limit_product()` in `feature/adm_gain_limit.h`, ADR-1413); VIF gain
  limiting uses fp32 `sycl::fmin`. **On rebase**: upstream cherry-pick
  bringing `double` into kernel lambda -> refactor to int64 / fp32
  before merging.
- **Scratch-free kernels ([ADR-1395](../../../../docs/adr/1395-sycl-kernels-no-scratch.md)).**
  Arc-series under Linux xe driver returns wrong values from kernels with
  scratch memory (private array in memory, register spill). No new kernel may
  use scratch. `test_sycl_kernel_scratch` (`--suite sycl`, needs Intel GPU)
  fails on any scratch kernel: `scratch_ratchet.txt` is EMPTY since
  2026-10-01 (`float_adm_sycl` CM kernels cleared, A380: 110 kernels, 0 with
  scratch) and `kScratchExtractors` in `scratch_check.cpp` = `""`. Never add
  line or name again; `test_sycl_kernel_source_contract.py` rejects both
  without device. Self-test warning then says no libvmaf extractor is
  affected.
  Kernel ids are mangled launcher names: renamed or re-typed launcher shows up
  as unlisted + unregistered, fix line. `vmaf_sycl_state_init` calls
  `vmaf_sycl_scratch_selftest()`: warning only, never refuses device, keep it
  that way. Probe kernel names `VmafSyclScratchProbePrivate` /
  `VmafSyclScratchProbeSpill` stay out of audit; probes must keep using scratch
  (test asserts private-array probe `private_bytes > 0`).
- **VAAPI / dmabuf zero-copy import surface
  ([ADR-0183](../../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md))**:
  `vmaf_sycl_import_va_surface` consumed by `libvmaf_sycl` FFmpeg
  filter (`ffmpeg-patches/0005-*.patch`). Symmetric to Vulkan VkImage
  import in
  [ADR-0186](../../../../docs/adr/0186-vulkan-image-import-impl.md).
  Public surface change touches patch file too — see CLAUDE.md §12
  r14.
