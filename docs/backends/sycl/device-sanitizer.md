# SYCL device AddressSanitizer

<!-- markdownlint-disable MD013 -->
How to build the SYCL backend with the DPC++ device AddressSanitizer, what it can and cannot
prove on an Intel Arc GPU, and the check that keeps the answer honest. Back to the
[SYCL overview](overview.md). Decision: [ADR-1930](../../adr/1930-sycl-device-asan-option.md).

## Short answer

- `-Dsycl_device_asan=true` instruments every SYCL kernel of the build. `-Dcpp_args=...` does not:
  the SYCL translation units are custom targets and never see it.
- On oneAPI 2026.0 with an Arc A380 the sanitizer works for kernels that capture their pointers
  directly and does not work for a pointer held in a struct captured by value, which is how most
  libvmaf kernels receive their planes. An instrumented libvmaf therefore returns wrong values or
  reports null-pointer accesses on correct code, and it cannot serve as a memory-safety check yet.
- The standing evidence for SYCL index and stride safety is the static check
  `core/test/test_gpu_byte_stride_contract.py` plus the parity tests against the CPU, which catch an
  over-read as a wrong score.

## Build

```text
source /opt/intel/oneapi/setvars.sh
CC=icx CXX=icpx meson setup build-asan core -Denable_sycl=true -Dsycl_icpx_aot_targets= \
    -Db_lto=false -Dsycl_device_asan=true
ninja -C build-asan
```

The option adds `-Xarch_device -fsanitize=address -g -O2` to every SYCL compile (extractors, tests,
the per-translation-unit AOT override) and to the link. `-O2` is spelled out because `-g` alone
turns icpx's default optimisation off, and an `-O0` instrumented kernel never finished on the A380.
Keep `-Dsycl_icpx_aot_targets=` empty: the measurements below are SPIR-V JIT. It is off by default
and refused with AdaptiveCpp and with an MSVC-syntax toolchain.

Run the binaries with `ONEAPI_DEVICE_SELECTOR=level_zero:gpu`. The Level Zero sanitizer layer
(`UR_LAYER_ASAN`) is loaded by the DPC++ runtime for a binary whose link carries the flag and prints
`==== DeviceSanitizer: ASAN` at start. Options go in `UR_LAYER_ASAN_OPTIONS` (Intel's guide documents
`quarantine_size_mb` and `redzone`; the installed loader also knows `debug`, `detect_kernel_arguments`,
`detect_leaks`, `detect_locals`, `detect_privates`, `halt_on_error` and `print_stats`).
`VMAF_SYCL_SCRATCH_SELFTEST=0` skips libvmaf's start-up scratch probe, which uses buffer accessors
the layer reports as an unknown device ([ADR-1395](../../adr/1395-sycl-kernels-no-scratch.md)).

## Why the earlier run reported nothing

Measured 2026-10-06 on master `1cf7d2d2c`, Arc A380 (Level Zero 1.17, driver 12.56.5), oneAPI
2026.0.0.20260331.

| Question | Finding |
| --- | --- |
| Was the layer loaded? | Yes: a build with the flag only in `cpp_args` still printed the banner, because the executable's link line carries the flag. |
| Were the kernels instrumented? | No. The compile line of `integer_motion_pipeline_sycl.cpp` under `-Dcpp_args` was `icpx ... -c -fsycl -std=c++20 -fp-model=precise ...` with no sanitizer flag (`ninja -t commands src/integer_motion_pipeline_sycl.o`). An uninstrumented kernel reports nothing, so the planted reads were invisible. This is the cause of the row. |
| Are staged planes sub-allocations of one block? | No. Every plane is its own `sycl::malloc_device` call (`vmaf_sycl_malloc_device`), the ring of `motion_v2` and the shared reference and distorted planes included. A read past a plane lands in the allocation's red zone. |
| Does the sanitizer see an overrun inside one block? | No. The probe reads the second plane of one two-plane block without a report (case `inside-block`); a read past the whole block is reported (`past-block`). Anything that packs planes into one allocation hides an overrun of one plane into the next. |

## What the sanitizer does and does not do here

The probe (`scripts/dev/sycl_device_asan_check.sh`, meson test `test_sycl_device_asan_probe`, suites
`gpu` and `sycl`) builds one kernel with the option's flags and runs six cases. Each case names its
expected outcome and the script fails on any other.

| Case | Expected | Meaning |
| --- | --- | --- |
| `clean` | no report | in-bounds reads pass |
| `past-allocation` | `out-of-bounds-access` | a read 64 elements past a USM block is caught: this is the planted case |
| `past-block` | `out-of-bounds-access` | a read past a two-plane block is caught |
| `inside-block` | no report | the blind spot above |
| `struct-pointer` | `null-pointer-access` | the toolchain limitation below |
| `no-sanitizer` | no report | the planted read built without the flag is silent: the negative control |

Compiling the probe's sanitized binary without the flag makes `past-allocation`, `past-block` and
`struct-pointer` fail, which is the original miss reproduced.

### The struct-capture limitation

A kernel that captures a struct holding pointers by value reads those pointers as null under the
sanitizer (`struct-pointer`; the same kernel without the flag returns the right sum). A pointer
captured on its own, as a lambda init-capture, is read correctly. libvmaf passes planes to kernels
in such structs (`SadArgs` in `integer_motion_pipeline_sycl.cpp` is one), so on the A380 an instrumented `test_sycl_psnr_parity` reports
`null-pointer-access`, `test_sycl_ssim_parity` returns a different SSIM and
`test_sycl_motion_v2_parity` a SAD of 0. Capturing the motion kernel's pointers one by one removed
the report but the SAD stayed 0 with no report; that was not traced further, and the change was not
kept. Treat an instrumented libvmaf as invalid on this toolchain until the `struct-pointer` case
stops reporting; then re-run the SYCL parity programs with the option and the planted reads of
`T-SYCL-DEVICE-SANITIZER-UNPROVEN-2026-10-05`.
