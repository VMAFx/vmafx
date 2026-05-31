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
See [ADR-0335](../adr/0335-adaptivecpp-second-sycl-toolchain.md) for
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
<https://adaptivecpp.github.io/AdaptiveCpp/installing.html>. The
fork's CI does not yet ship an official AdaptiveCpp lane (a future
PR adds `.github/workflows/sycl-acpp.yml` per ADR-0335 § follow-ups).

### Build the fork with AdaptiveCpp

```bash
meson setup build-acpp \
    -Denable_cuda=false \
    -Denable_sycl=true \
    -Dsycl_compiler=acpp \
    -Dsycl_acpp_targets=generic
ninja -C build-acpp
```

`-Dsycl_acpp_targets` accepts any AdaptiveCpp `--acpp-targets`
string. Common values:

| Value | Meaning |
|---|---|
| `generic` | Single-source SPIR-V — runs on any SPIR-V-capable runtime. **Recommended default.** |
| `omp` | OpenMP CPU only — useful for CI runners without GPUs. |
| `omp;cuda:sm_75` | CPU + NVIDIA CUDA (Turing). |
| `omp;hip:gfx1100` | CPU + AMD HIP (RDNA3). |

## Quickstart — Intel oneAPI (default)

See [`oneapi-install.md`](oneapi-install.md). The default invocation
is unchanged:

```bash
meson setup build -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

`sycl_compiler` defaults to `icpx`; nothing else needs to change.

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
| `[[intel::reqd_sub_group_size(N)]]` | yes (verbatim) | **no — neutralised by `VMAF_SYCL_REQD_SG_SIZE(N)` macro** | AdaptiveCpp picks sub-group size per backend at JIT time. The macro reduces to a no-op under acpp; see `core/src/feature/sycl/sycl_compat.h`. |
| `sycl::ext::oneapi::experimental::*` | yes | no | Intel-specific extensions. The fork uses **none** today. |
| `joint_matrix` | yes | partial / target-dependent | The fork uses none. |
| Level Zero zero-copy import (`get_native<ext_oneapi_level_zero>`) | yes | conditional — works only when targeting an Intel L0 backend under acpp | Defaults to icpx-only in practice; AdaptiveCpp on non-Intel HW falls back to host-staged copies. |
| DMA-BUF / VAAPI surface import | yes | yes (Linux only, `--acpp-targets=generic` or L0 path) | The build plumbing wires `libva` + `libva-drm` for both toolchains. |
| D3D11 staging-texture surface import | yes (Windows) | untested | Out of scope for AdaptiveCpp on the fork as of 2026-05-08. |

## Numerical conformance

**AdaptiveCpp output is not bit-identical to icpx, and not
bit-identical to scalar CPU.** This is consistent with the fork's
[golden-gate-CPU-only rule](../../CLAUDE.md#8-netflix-golden-data-gate-do-not-modify):
no GPU / SYCL backend is bit-identical to the Netflix CPU golden
assertions, only "close enough" within `places=4`. AdaptiveCpp adds
another non-bit-identical lane to that family.

The build replaces `-fp-model=precise` (an icpx-specific strict-FP
flag) with `-ffp-contract=off` (which AdaptiveCpp's underlying clang
accepts). This blocks FMA contraction in the kernel lambdas — the
load-bearing invariant per
[`core/src/sycl/AGENTS.md`](../../core/src/sycl/AGENTS.md) §
"`-fp-model=precise` is load-bearing".

When a future PR extends the cross-backend ULP-tolerance gate
([`/cross-backend-diff` skill](../../.claude/skills/cross-backend-diff/))
to cover acpp, that PR adds the per-feature ULP entries for the
acpp CPU OpenMP backend.

## CI implications

CI runners without Intel hardware are today limited to either (a)
self-hosted runners with Intel iGPU/Arc, or (b) Intel CPU OpenCL
under icpx (a CPU-emulated GPU path). AdaptiveCpp's
`--acpp-targets=omp` adds a third option: pure OpenMP CPU
execution that runs anywhere LLVM does, including stock
`ubuntu-latest`.

A follow-up PR (`.github/workflows/sycl-acpp.yml`, sized ~50 LOC in
ADR-0335 § follow-ups) will land that lane as a non-required
status check before promoting it to `required-aggregator.yml`.

## Troubleshooting

### `find_program('acpp')` fails

The configured `sycl_compiler` is not on `PATH`. Either install
AdaptiveCpp into a system path, or pass the absolute path:

```bash
meson setup build-acpp \
    -Dsycl_compiler=/opt/adaptivecpp/bin/acpp \
    -Dsycl_acpp_targets=generic \
    -Denable_sycl=true
```

### `cannot find -lacpp-rt`

The runtime library lives next to the compiler driver under
`<acpp-prefix>/lib`. The build derives this from the resolved
`acpp` binary's `bindir`. If the install layout is non-standard,
the legacy `libhipSYCL-rt.so` is also probed as a fallback. If
neither name resolves, file an issue with the AdaptiveCpp install
layout — the fork supports the upstream layout, not custom ones.

### Kernel runs but produces different scores than icpx

Expected. See § "Numerical conformance" above. The acceptance bar
is `places=4` against the Netflix golden CPU values, not bit-exact
parity with icpx.

## See also

- [ADR-0335](../adr/0335-adaptivecpp-second-sycl-toolchain.md) — the
  design decision.
- [ADR-0217](../adr/0217-sycl-toolchain-cleanup.md) — multi-version
  oneAPI install recipe (icpx side).
- [ADR-0220](../adr/0220-sycl-fp64-fallback.md) — fp64-free kernel
  contract (preserved under both toolchains).
- [`oneapi-install.md`](oneapi-install.md) — Intel oneAPI install.
- [Research-0086](../research/0086-sycl-toolchain-audit-2026-05-08.md)
  § Topic B — the audit that recommended this work.
