<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1930: `sycl_device_asan` puts the device sanitizer on every SYCL compile and the link

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: Lusoris
- **Tags**: sycl, build, testing, sanitizer, fork-local

## Context

`T-SYCL-DEVICE-SANITIZER-UNPROVEN-2026-10-05` recorded a SYCL build with the DPC++ device
AddressSanitizer in which all 44 SYCL parity programs passed and neither of two planted reads past a
plane in `integer_motion_pipeline_sycl.cpp` was reported. The SYCL translation units are Meson
custom targets, so `-Dcpp_args=...` never reaches their command line: such a build instruments no
kernel, while the executable's link line still loads the sanitizer layer and prints its banner. The
run looked sanitized and was not. Giving the flag to every SYCL compile and the link instruments the
kernels, and on the Arc A380 with oneAPI 2026.0 the instrumented libvmaf kernels then fail for a
second reason: a pointer held in a struct captured by value reads as null, so the library cannot be
run end to end under the sanitizer (details and measurements in
[the device sanitizer page](../backends/sycl/device-sanitizer.md)).

## Decision

We add the build option `sycl_device_asan` (boolean, default false, icpx on Linux only). It routes one
list, `sycl_asan_args` (`-Xarch_device -fsanitize=address -g -O2`), into `sycl_toolchain_args`, the
per-translation-unit override of the AOT skip table and `sycl_link_args`; `-O2` is spelled out because
`-g` alone drops icpx's default to `-O0`, at which an instrumented kernel never completes on the
A380. `scripts/dev/sycl_device_asan_check.sh` (meson test `test_sycl_device_asan_probe`, suites `gpu`
and `sycl`) runs a probe kernel with six cases, each with its expected outcome, among them the
planted read past an allocation that must be reported and the same read without the flag that must
not. `core/test/test_sycl_device_asan_option_contract.py` (fast) guards the routing with a planted
negative per route.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Document `-Dcpp_args` as the way | No build change | Instruments nothing; the failure this row recorded | Wrong in practice |
| Capture each pointer separately in the motion kernels so the library can be sanitized | Would prove the planted read on the real kernel | Measured: the motion kernel then returns a SAD of 0 under the sanitizer with no report, so the library still cannot be proven; changes a production kernel for a debug aid | Not enough gain |
| Skip the option, keep only the standalone probe | Smaller | Nobody can instrument the library when a later oneAPI fixes the struct capture | The option is 22 lines and off by default |

## Consequences

- **Positive**: the row's question has a reproducible answer; a later oneAPI that fixes the struct
  capture can be retried with one option; the probe fails if the flags stop instrumenting.
- **Negative**: an instrumented libvmaf is not a usable check on this toolchain, so the static
  byte-stride check (`test_gpu_byte_stride_contract.py`) remains the SYCL evidence.
- **Neutral / follow-ups**: re-run the probe after each oneAPI upgrade; its `struct-pointer` case
  starts failing when the toolchain limitation is gone.

## References

- Request (paraphrased): find out why device AddressSanitizer missed two planted reads, whether the
  sanitizer layer is loaded and whether staged planes are sub-allocations of one block, then repeat
  the planted case and record either a reproducible check or the documented reason it cannot catch it.
- Row `T-SYCL-DEVICE-SANITIZER-UNPROVEN-2026-10-05` in [docs/state.md](../state.md);
  [ADR-1395](1395-sycl-kernels-no-scratch.md).
