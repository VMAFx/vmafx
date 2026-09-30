<!-- markdownlint-disable MD013 MD060 -->
# ADR-1395: SYCL kernels use no scratch memory on Intel GPUs

- **Status**: Accepted
- **Date**: 2026-10-01
- **Deciders**: lusoris
- **Tags**: sycl, gpu, numerics, testing, rc3, fork-local

## Context

A kernel uses scratch memory when the Intel graphics compiler (IGC) places a
private array in memory (`kernel_device_specific::private_mem_size` > 0) or
spills registers (`spill_memory_size` > 0). On the Arc A380 (`dg2-g11`, PCI ID
56a5) under the Linux **xe** kernel driver (kernel 7.2.8, compute runtime
26.35.39758.10, IGC 2.41.5), such kernels return wrong values. Two standalone
probes with no vmafx code show it: a kernel that indexes a 1 KiB private array
got 256 of 256 work-items wrong, and a SIMD-32 kernel that spills 4 KiB per
thread got 256 of 256 wrong. Both are correct on the OpenCL CPU device and in
the same kernels without scratch.

The fork's SYCL suite fails 16 tests on this host, and each of them runs a
kernel with scratch. 25 of the 109 kernels registered by master `10f27efe2`
use scratch on the A380. Under the i915 driver, with the same runtime and
compiler, 13 of those 16 tests passed on 2026-09-25 (the other three did not
exist yet). The A380 stays on xe, which is also the driver newer Intel GPUs use
by default; see [Research-1395](../research/1395-sycl-kernels-no-scratch.md).

A kernel that is correct on one driver and silently wrong on another cannot be
caught by the parity tests alone: they only fail on a host that has the defect,
and they do not say why.

## Decision

SYCL kernels in libvmaf use no private memory and no register spills on Intel
GPUs.

- **Rule.** A kernel whose working set does not fit the default 128-entry
  register file at its SIMD width moves data to local memory, is restructured,
  or asks for the 256-entry register file through `VmafSyclKernelShape<SG, 256>`
  in `core/src/feature/sycl/sycl_compat.h` (`sycl_ext_intel_grf_size`).
- **Ratchet.** `test_sycl_kernel_scratch` (`--suite sycl`) builds every kernel
  registered in the program for the default GPU and fails when a kernel that is
  not in `core/src/sycl/scratch_ratchet.txt` uses scratch. A listed kernel that
  no longer does prints a note to delete its line. The list holds the 15 kernels
  that use scratch on the A380 after this change, run by 7 extractors (with
  `adm_sycl` and `psnr_hvs_sycl` cleared in #1656 and #1657), and it
  only shrinks.
- **Self-test.** `vmaf_sycl_state_init` runs a private-array probe and a SIMD-32
  spill probe once per device and process. When either returns wrong values it
  logs a warning that names the xe defect and the extractors on the ratchet
  list. It never refuses the device. `VMAF_SYCL_SCRATCH_SELFTEST=0` skips it.
- **First fix.** `integer_vif_sycl`'s SIMD-32 horizontal and fused kernels
  request the 256-entry register file. They spilled 96 to 8832 bytes per thread
  and now spill nothing. `VMAF_SYCL_VIF_SUBGROUP_SIZE=16|32` forces a sub-group
  size, because an Intel GPU never selects SIMD-32 on its own.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Refuse the SYCL device when the self-test fails | No wrong score can come from a defective device | Also refuses the extractors whose kernels are scratch-free and correct on xe; the A380 host stays on xe | A warning names the affected extractors and keeps the working ones |
| Route only the affected extractors to the CPU on a defective device | Correct scores without user action | Needs a per-extractor capability table kept in sync with kernel changes; changes timing silently | Removing the scratch use fixes the cause; the ratchet list is the table for the warning |
| Large register file for every kernel (`-ze-opt-large-register-file`) | One build flag; removes every spill and one private array on the A380 (25 scratch kernels to 14) | Halves hardware threads per EU for kernels that do not need it; leaves 14 kernels with private arrays; JIT-only when set through `SYCL_PROGRAM_COMPILE_OPTIONS` | Per-kernel choice through `VmafSyclKernelShape` costs occupancy only where registers run out |
| Check AOT images statically (IGC zeinfo) at build time | Runs in CI without a GPU | Covers only the configured AOT targets and not the JIT image; needs `ocloc disasm` per target | The runtime audit measures the device that runs the code and both image kinds |
| Fix every scratch kernel in this change | No ratchet list | 17 kernels in 9 extractors, each with its own parity and timing work, some already in flight in other branches | The ratchet stops new scratch now and tracks the rest |

## Consequences

- **Positive**: a kernel that starts to use scratch fails the SYCL suite on any
  host with an Intel GPU; a user on a defective driver gets a warning that names
  the affected extractors; `vif_sycl` at SIMD-32 now scores correctly on the
  A380 under xe (it failed at frame 0 before).
- **Negative**: the audit needs a GPU, so CI, which has none, skips it. The
  ratchet list keys on mangled kernel names: changing a launcher's signature
  makes the test report the kernel as unlisted and its old line as unregistered,
  and the PR updates the line. The self-test adds about 0.4 s to the first SYCL
  initialisation on a cold JIT cache and 3 to 5 ms on a warm one. The 256-entry
  register file halves the hardware threads per EU for the two `vif` SIMD-32
  kernels, which only run when forced.
- **Neutral / follow-ups**: the remaining ratchet entries (`psnr_hvs`, integer
  `adm`, `float_vif`, `float_adm`, SpEED, 16-bit `motion`, `cambi`) are tracked
  in `T-SYCL-XE-SCRATCH-WRONG-RESULTS-2026-10-01`; every PR that clears one
  deletes its line and the extractor from `vmaf_sycl_scratch_extractors()`.

## References

- [Research-1395](../research/1395-sycl-kernels-no-scratch.md): probe matrix,
  kernel audit, register-file experiment and the `vif` SIMD-32 measurements.
- [ADR-0220](0220-sycl-fp64-fallback.md): the other device-portability rule the
  SYCL kernels follow.
- [ADR-0488](0488-gpu-dispatch-env-shared-snapshot.md): the environment snapshot
  the two new variables read through.
- Source: RC3 handoff for the Arc A380 host (issue #1641), paraphrased: the A380
  stays on the xe driver; the SYCL kernels become scratch-free, guarded by a
  ratchet test and a warning-only self-test.
