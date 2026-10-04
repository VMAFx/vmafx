<!-- markdownlint-disable MD060 -->
# SYCL toolchain options — Intel oneAPI vs AdaptiveCpp

The fork's `-Denable_sycl=true` build path supports **two** SYCL
toolchains:

| Toolchain | Default? | Install size | Source | Use case |
|---|---|---|---|---|
| Intel oneAPI `icpx` | yes | ~2.6 GB | closed-binary | Production builds, Intel hardware (iGPU, Arc, Battlemage), OpenVINO / NPU enablement. |
| AdaptiveCpp `acpp` | no | ~50 MB | open-source (BSL) | Contributor builds without Intel hardware, second-toolchain CI lane, AMD HIP / NVIDIA CUDA SYCL targets. |

Both use the same `core/src/feature/sycl/*.cpp` kernels — the
build plumbing branches on the configured `sycl_compiler` basename.
See [ADR-0407](../adr/0407-adaptivecpp-second-sycl-toolchain.md) for
the design rationale.

## Quickstart — AdaptiveCpp

### Arch / CachyOS

AdaptiveCpp is packaged in the AUR as `adaptivecpp`. The version
pinned for the initial fork support is **25.10.0** (AUR
`adaptivecpp` 25.10.0-2 as of 2026-05-08). It is **not** in the
official `extra` repository; an AUR helper or a manual `makepkg`
build is required.

```bash
# With paru / yay:
paru -S adaptivecpp

# Or manual:
git clone https://aur.archlinux.org/adaptivecpp.git
cd adaptivecpp && makepkg -si
```

Verify:

```bash
acpp --version  # → AdaptiveCpp version: 25.10.0
```

### Other distros / from source

AdaptiveCpp builds against any modern LLVM (≥ 16). Upstream
instructions live at
<https://adaptivecpp.github.io/AdaptiveCpp/installing.html>. No CI lane builds
with AdaptiveCpp yet (see [CI implications](#ci-implications)).

### Build the fork with AdaptiveCpp

```bash
meson setup build-acpp core \
    -Denable_cuda=false \
    -Denable_sycl=true \
    -Dsycl_compiler=acpp \
    -Dsycl_acpp_targets=generic \
    -Dcompress_device_code=false
ninja -C build-acpp
```

`-Dcompress_device_code=false` is required: AdaptiveCpp has no device image
compression, and the option's default (`true`) makes configure stop with an
error that says so rather than build uncompressed without telling you
([`compress_device_code`](build-flags.md#compress_device_code)).

`-Dsycl_acpp_targets` accepts any AdaptiveCpp `--acpp-targets`
string. Common values:

| Value | Meaning |
|---|---|
| `generic` | Single-source SPIR-V — runs on any SPIR-V-capable runtime. **Recommended default.** |
| `omp` | OpenMP CPU only — useful for CI runners without GPUs. |
| `omp;cuda:sm_80` | CPU + NVIDIA CUDA (Ampere; sm_75 and older are unsupported per ADR-1223). |
| `omp;hip:gfx1100` | CPU + AMD HIP (RDNA3). |

## Quickstart — Intel oneAPI (default)

See [`oneapi-install.md`](oneapi-install.md). From the repository root:

```bash
meson setup build core -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

`sycl_compiler` defaults to `icpx`. The default `sycl_icpx_aot_targets` list
compiles every kernel ahead of time for 19 Intel targets and needs Intel's
`ocloc` on `PATH`; `-Dsycl_icpx_aot_targets=` (empty) falls back to SPIR-V
just-in-time compilation without `ocloc`
([ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md)).

## Capability matrix

The fork's SYCL feature kernels exercise the SYCL 2020 surface
listed below. AdaptiveCpp coverage cited from
<https://adaptivecpp.github.io/AdaptiveCpp/>; Intel oneAPI is the
reference implementation against which the fork is bit-identity
tested.

| Feature | icpx (default) | AdaptiveCpp `acpp` | Notes |
|---|---|---|---|
| `sycl::queue`, `nd_range`, `parallel_for` | yes | yes | Core SYCL 2020. |
| `sycl::usm` (`malloc_device`, `malloc_host`, `memcpy`) | yes | yes | All targets. |
| `sycl::local_accessor` | yes | yes | All targets. |
| `sycl::sub_group`, `reduce_over_group` | yes | yes | CUDA / HIP / SPIR-V. |
| `sycl::atomic_ref<int64, relaxed, device, global>` | yes | yes | int64 atomics on older AMD HIP devices may need a fallback at HIP target build time. |
| `[[sycl::reqd_sub_group_size(N)]]` through `VMAF_SYCL_REQD_SG_SIZE(N)` | yes, N is 16 or 32 | no (the macro expands to nothing) | See note 1. |
| `sycl::ext::oneapi::experimental` (kernel properties, `grf_size`) | yes | no | See note 2. |
| `joint_matrix` | yes | partial / target-dependent | The fork uses none. |
| Level Zero zero-copy import (`get_native<ext_oneapi_level_zero>`) | yes | conditional — works only when targeting an Intel L0 backend under acpp | Defaults to icpx-only in practice; AdaptiveCpp on non-Intel HW falls back to host-staged copies. |
| DMA-BUF / VAAPI surface import | yes | yes (Linux only, `--acpp-targets=generic` or L0 path) | The build plumbing wires `libva` + `libva-drm` for both toolchains. |
| D3D11 staging-texture surface import | yes (Windows) | untested | Out of scope for AdaptiveCpp on the fork as of 2026-05-08. |

Notes:

1. `VMAF_SYCL_REQD_SG_SIZE(N)` in `core/src/feature/sycl/sycl_compat.h`
   expands to `[[sycl::reqd_sub_group_size(N)]]` under icpx (the
   `[[intel::...]]` spelling is deprecated since oneAPI 2026.0) and to nothing
   under AdaptiveCpp, which picks the sub-group size per backend at JIT time.
   N must be 16 or 32: the Xe2 targets of the default AOT list accept no other
   size ([ADR-1468](../adr/1468-sycl-sub-group-sizes-every-aot-target.md)).
2. The fork uses the experimental kernel-properties extension for the large
   register file (`VmafSyclKernelShape` in `sycl_compat.h`,
   [ADR-1395](../adr/1395-sycl-kernels-no-scratch.md)) and in
   `core/src/sycl/common.cpp`; the AdaptiveCpp build compiles those paths out.

## Numerical conformance

Under icpx, every SYCL twin declared exact returns the CPU extractor's scores
bit for bit; the declarations are the `scripts/ci/exact_twins.d/*.sycl`
fragments, listed in the generated
[exact-twins table](cross-backend-exact-twins.md). AdaptiveCpp builds are not
measured against that gate: their output is not known to be bit-identical to
icpx or to the CPU. The Netflix golden assertions are checked on the CPU only
([ADR-0024](../adr/0024-netflix-golden-preserved.md)).

Under icpx every SYCL feature TU compiles with
`-fp-model=precise -ffp-contract=off -foffload-fp32-prec-div
-foffload-fp32-prec-sqrt`: no fused multiply-add and correctly rounded fp32
division and square root, as on the CPU
([ADR-1367](../adr/1367-sycl-strict-fp-every-feature-tu.md)). AdaptiveCpp
accepts neither `-fp-model` nor the precision pair, so its line is
`-ffp-contract=off` alone. That blocks FMA contraction in the kernel
lambdas, but device division and square root keep whatever precision the
AdaptiveCpp backend gives them. See
[`core/src/sycl/AGENTS.md`](../../core/src/sycl/AGENTS.md) §
"SYCL strict FP line load-bearing".

No CI lane compares AdaptiveCpp output with the CPU yet. Extending the
cross-backend gate ([`/cross-backend-diff`
skill](../../.claude/skills/cross-backend-diff/))
to acpp would add its per-feature entries.

## CI implications

No workflow builds with AdaptiveCpp today. Without Intel hardware, a CI runner
can run SYCL code in two ways:

1. a self-hosted runner with an Intel iGPU or Arc card;
2. Intel's CPU OpenCL runtime under icpx, which emulates a GPU on the CPU.

AdaptiveCpp's `--acpp-targets=omp` would add a third: plain OpenMP on the CPU,
which runs anywhere LLVM does, including stock `ubuntu-latest`. ADR-0407 lists
`.github/workflows/sycl-acpp.yml` as a follow-up: a non-required check first,
promoted to `required-aggregator.yml` later.

## Troubleshooting

### `find_program('acpp')` fails

The configured `sycl_compiler` is not on `PATH`. Either install
AdaptiveCpp into a system path, or pass the absolute path:

```bash
meson setup build-acpp core \
    -Dsycl_compiler=/opt/adaptivecpp/bin/acpp \
    -Dsycl_acpp_targets=generic \
    -Denable_sycl=true \
    -Dcompress_device_code=false
```

### `cannot find -lacpp-rt`

The runtime library lives next to the compiler driver under
`<acpp-prefix>/lib`. The build derives this from the resolved
`acpp` binary's `bindir`. If the install layout is non-standard,
the legacy `libhipSYCL-rt.so` is also probed as a fallback. If
neither name resolves, file an issue with the AdaptiveCpp install
layout — the fork supports the upstream layout, not custom ones.

### Kernel runs but produces different scores than icpx

Expected: AdaptiveCpp output is not measured against the exact-twin gate. See
[Numerical conformance](#numerical-conformance).

## See also

- [ADR-0407](../adr/0407-adaptivecpp-second-sycl-toolchain.md) — the
  design decision.
- [ADR-0217](../adr/0217-sycl-toolchain-cleanup.md) — multi-version
  oneAPI install recipe (icpx side).
- [ADR-0220](../adr/0220-sycl-fp64-fallback.md) — fp64-free kernel
  contract (preserved under both toolchains).
- [`oneapi-install.md`](oneapi-install.md) — Intel oneAPI install.
- [Research-0086](../research/0086-sycl-toolchain-audit-2026-05-08.md)
  § Topic B — the audit that recommended this work.
