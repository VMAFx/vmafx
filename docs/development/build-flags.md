# Meson build flags

Every build-time option of libvmaf is listed here: what it enables, its
default, what it depends on and what changes at run time. Options come from
[`core/meson_options.txt`](../../core/meson_options.txt); a short section on the
standard Meson options that change the artifact follows.

Per [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md), build flags are
a user-discoverable surface and ship documentation in the same PR as the code.

## Quick start

Run Meson from the repository root; the Meson source directory is `core/`.
Pass an option with `-D<name>=<value>`:

```bash
meson setup build core -Denable_cuda=true -Denable_avx512=false
ninja -C build
```

Reconfigure an existing build tree without wiping it:

```bash
meson configure build -Denable_sycl=true
ninja -C build
```

Show the resolved configuration of a build tree:

```bash
meson configure build | head -40
```

The project sets `buildtype=release`, `default_library=both`, `b_lto=true` and
`b_lto_threads=4` as defaults in `core/meson.build`; every other option
defaults as the tables below say.

## Recommended configurations

Fast iteration, CPU only, assertions on, AVX2 and AVX-512 kept:

```bash
meson setup build core \
  -Dbuildtype=debugoptimized \
  -Denable_cuda=false -Denable_sycl=false
```

Release build with CUDA and NVTX ranges for profiling:

```bash
meson setup build core \
  -Dbuildtype=release \
  -Denable_cuda=true -Denable_nvtx=true \
  -Denable_sycl=false
```

Netflix CPU golden-gate build: use the make target, which configures an
isolated build directory (`core/build-golden`) with a fixed option set and
refuses any compiler other than `gcc` or `clang`
([ADR-1317](../adr/1317-golden-gate-build-isolation.md)):

```bash
make build-golden
```

The option set is in
[`scripts/ci/setup-golden-build.sh`](../../scripts/ci/setup-golden-build.sh):
`--buildtype release` with `enable_float=true`, `enable_cuda=false`,
`enable_sycl=false`, `enable_hip=false`, `enable_dnn=disabled`,
`enable_tests=false` and `enable_docs=false`.

Tiny-AI tests (CI) need ONNX Runtime to link, so fail the configure step when
it is missing:

```bash
meson setup build core -Dbuildtype=release -Denable_dnn=enabled
```

Sanitizer run (what `make test` uses):

```bash
meson setup build core -Dbuildtype=debug -Db_sanitize=address,undefined
```

## Project options

All options of `core/meson_options.txt`, grouped by what they build. The
Effect column is one sentence; options with more to say link to a section
under [Option details](#option-details).

### Build content

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_tests` | bool | `true` | Build the `core/test/` unit tests; the credential-safe Meson runner needs them. |
| `enable_docs` | bool | `true` | Build the Doxygen C-API HTML under `build/core/doc/`. |
| `enable_tools` | bool | `true` | Build the `vmaf` and `vmaf_bench` CLI binaries. |
| `built_in_models` | bool | `true` | Compile the default `.json` VMAF models into the library, so `version=vmaf_v0.6.1` and the like resolve without disk I/O. |
| `enable_float` | bool | `true` | Compile the `float_*` extractors; see [`enable_float`](#enable_float). |
| `fuzz` | bool | `false` | Build the libFuzzer harnesses under `core/test/fuzz/`; see [`fuzz`](#fuzz). |
| `enable_rust_features` | bool | `false` | Build the Rust extractors (Rust twins, TAD); see [`enable_rust_features`](#enable_rust_features). |

### SIMD

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_asm` | bool | `true` | Compile the `*.asm` sources (nasm); `false` disables every SIMD path. |
| `enable_avx512` | bool | `true` | Build the AVX-512 kernels (nasm 2.14 or newer); downgraded automatically when the toolchain or host headers cannot build them. |

### CUDA

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_cuda` | bool | `false` | Compile the CUDA backend and its `.cu` kernels; needs the CUDA toolkit (`nvcc`). |
| `enable_nvtx` | bool | `false` | Instrument CUDA kernels with NVTX ranges for Nsight Systems; see [backends/nvtx/profiling.md](../backends/nvtx/profiling.md). |
| `enable_zimg` | bool | `false` | Build `vmaf_picture_convert()` (colourspace, format and size conversion) against zimg >= 2.7 found by pkg-config; off, the function returns `-ENOTSUP`. See [Pictures](../api/pictures.md#converting-pictures-vmaf_picture_convert) and [ADR-1822](../adr/1822-additive-picture-convert.md). |
| `enable_nvcc` | bool | `true` | Compile the CUDA kernel objects with `nvcc` instead of the clang CUDA driver; only read when `enable_cuda=true`. |
| `nvcc_threads` | integer | `4` | `nvcc --threads` for the per-kernel fatbin compiles, range 1 to 32; see [`nvcc_threads`](#nvcc_threads). |

### SYCL

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_sycl` | bool | `false` | Compile the SYCL backend and its DPC++ kernels; needs `icpx` on `PATH`, or `acpp` / `syclcc` with `sycl_compiler=acpp` ([ADR-0407](../adr/0407-adaptivecpp-second-sycl-toolchain.md)). |
| `sycl_compiler` | string | `icpx` | Path or name of the SYCL compiler: Intel `icpx` or AdaptiveCpp `acpp` / `syclcc`; only read when `enable_sycl=true`. |
| `sycl_acpp_targets` | string | `generic` | AdaptiveCpp `--acpp-targets` value, for example `generic`, `omp`, `omp;cuda:sm_80` or `omp;hip:gfx1100`; ignored when `sycl_compiler=icpx`. |
| `sycl_icpx_aot_targets` | string | 19 Intel targets | Intel ahead-of-time device list for `icpx`; see [`sycl_icpx_aot_targets`](#sycl_icpx_aot_targets). |

### HIP (AMD ROCm)

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_hip` | bool | `false` | Compile the HIP backend; 19 extractors are registered; see [`enable_hip` and `enable_hipcc`](#enable_hip-and-enable_hipcc). |
| `enable_hipcc` | bool | `false` | Compile the real HIP kernels with `hipcc`; pair with `enable_hip=true`. |
| `hip_gfx_targets` | string | auto-detect | AMD GFX targets for `hipcc --offload-arch`; see [`hip_gfx_targets`](#hip_gfx_targets). |
| `enable_float_vif_hip_autodispatch` | bool | `true` | Let the model registry pick `float_vif_hip` on its own; see [`enable_float_vif_hip_autodispatch`](#enable_float_vif_hip_autodispatch). |

### GPU device code (CUDA, HIP, SYCL)

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `compress_device_code` | bool | `true` | Store every backend's device code compressed at the toolchain's strongest setting; see [`compress_device_code`](#compress_device_code). |

### Metal (Apple Silicon)

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_metal` | feature | `auto` | Build the Metal backend with its 17 feature extractors; see [`enable_metal`](#enable_metal). |

### Tiny-AI and MCP

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_dnn` | feature | `auto` | Build the ONNX Runtime tiny-AI surface; see [`enable_dnn`](#enable_dnn). |
| `enable_mcp` | bool | `false` | Compile the embedded MCP server into libvmaf; see [Embedded MCP options](#embedded-mcp-options). |
| `enable_mcp_sse` | feature | `auto` | Compile the SSE (loopback HTTP) transport; needs `enable_mcp=true`. |
| `enable_mcp_uds` | bool | `false` | Compile the Unix-domain-socket transport; needs `enable_mcp=true`. |
| `enable_mcp_stdio` | bool | `false` | Compile the stdio transport; needs `enable_mcp=true`. |

!!! note "`enable_vulkan` no longer exists"
    The Vulkan backend and its option were removed
    ([ADR-0726](../adr/0726-drop-vulkan-backend.md)). Passing
    `-Denable_vulkan=...` fails the configure step with Meson's
    `Unknown options` error, because Meson rejects a `-D` option the project
    does not declare. The FFmpeg patch series keeps a no-op shim for the
    option ([ADR-0860](../adr/0860-ffmpeg-patch-chain-no-op-vulkan-shim.md)).

## Option details

Options whose behaviour needs more than one sentence.

### `enable_float`

Compiles the `float_psnr`, `float_ssim`, `float_ms_ssim`, `float_vif`,
`float_adm` and `float_motion` extractors, so `--feature float_adm` and related
CLI flags work without extra configure flags. `float_ansnr` was removed
([ADR-0865](../adr/0865-ansnr-sunset-pre-vmaf-metric-drop.md)).

The integer extractors always compile; this flag only adds the float twins.
`speed_chroma` and `speed_temporal` are float extractors but compile whatever
this flag says, as in Netflix/vmaf: the `vmaf_v1.0.16` models, the default
model among them, read `speed_chroma`, so a `-Denable_float=false` build still
scores with the default model. Set `-Denable_float=false` only on
size-constrained embedded targets.

### `nvcc_threads`

`nvcc` parallelises across the six gencode architectures, which shortens the
critical-path kernel: 8.5 s to 2.1 s on `adm_cm.cu` at the default of `4`, with
all 22 fatbins byte-identical to a serial build
([ADR-1224](../adr/1224-cuda-tile-not-adopted.md)).

Ninja already builds the 22 fatbins concurrently, so the effective thread
count is this value times the ninja job count: lower it on a small runner.
`1` disables the feature, because `nvcc` ignores `--threads 1`.

### `sycl_icpx_aot_targets`

A comma-separated list of Intel device codenames. Each `icpx` compile gets
`-fno-sycl-rdc -fsycl-targets=spir64_gen,spir64
-Xsycl-target-backend=spir64_gen '-device <list>'`, after the compression
flags of [`compress_device_code`](#compress_device_code). Scoping the list to
`spir64_gen` keeps the portable `spir64` fallback free of warnings.

The default covers Arc dGPU and every common Intel iGPU generation from Tiger
Lake to Battlemage (19 targets). The value is one of:

| Value | Result |
| --- | --- |
| Default list | Native image for every listed device; needs Intel `ocloc` on `PATH` (`scripts/ci/install-intel-ocloc.sh`), and configure fails without it. |
| One target, for example `dg2-g11` | A smaller fat binary for a known fleet. |
| Empty string | SPIR-V JIT: smaller binary, slower first launch, no `ocloc` needed. |

On Linux shared builds the build also fails unless `libvmaf.so` carries a
native image for every listed target. The option is ignored when
`sycl_compiler` is not `icpx`. See
[ADR-0568](../adr/0568-sycl-icpx-aot-targets-default.md) and
[ADR-1360](../adr/1360-sycl-aot-compile-time-device-codegen.md); the install
steps are in [oneapi-install.md](oneapi-install.md).

### `compress_device_code`

The GPU kernels are embedded in `libvmaf` and, through `libvmaf.a`, in every
test program. With this option on (the default) every device compiler stores
them compressed at its strongest setting; the driver or runtime decompresses a
module when it loads it, and the code the GPU runs is the same
([ADR-1590](../adr/1590-device-code-compression.md)):

| Backend | Flags | Device code, before and after |
| --- | --- | --- |
| CUDA | `-Xfatbin=-compress-all --compress-mode=size`: every cubin and PTX entry of a fatbin (nvcc alone compresses only the PTX) | 21 fatbins: 10.97 MB to 2.60 MB; `libvmaf.so` 14.2 MB to 5.9 MB |
| HIP | `--offload-compress --offload-compression-level=22`: a zstd-compressed code object bundle | 25 targets: 17.9 MB to 1.09 MB; `libvmaf.so` 21.0 MB to 4.2 MB |
| SYCL (`icpx`) | `--offload-compress --offload-compression-level=22` on the AOT compile line and on the link that generates the SPIR-V image | 19 AOT targets: 6.42 MB to 5.23 MB, SPIR-V 1.90 MB to 0.44 MB; `libvmaf.so` 12.8 MB to 10.2 MB |

Module loading is not measurably slower: 1.4 ms against 3.0 ms for all 21
CUDA fatbins, and a one-frame `vmaf` run with 15 GPU twins takes the same time
on all three backends.

What a build needs:

- CUDA: `nvcc` 12.8 or newer (it added `--compress-mode`). The output loads on
  drivers from CUDA 12.4 (R550) on; a CUDA 13 build needs R580 anyway.
- HIP: a ROCm clang with `--offload-compression-level` (LLVM 19 on, so every
  ROCm 7 release). The HIP runtime decompresses the bundle.
- SYCL: `icpx` with `--offload-compression-level`.

Configure stops with an error when a compiler in use cannot compress: the
clang CUDA driver (`enable_nvcc=false`), AdaptiveCpp (`sycl_compiler=acpp`),
or a toolchain without the flags. Such builds pass
`-Dcompress_device_code=false`; so does anyone who wants the raw images, for
example to read them with tools that do not decompress. With the option off,
nvcc stores every entry raw (`--no-compress`) and hipcc and `icpx` store plain
images.

The build checks its own output: `core/src/check_device_compression.py` fails
it when a fatbin holds a raw cubin or PTX entry, a HIP bundle is not a
compressed (`CCOB`) bundle, or a SYCL image section of `libvmaf.so` (Linux
shared builds) is not zstd-compressed.

### `enable_hip` and `enable_hipcc`

`enable_hip=true` compiles the HIP (AMD ROCm) backend with its 19 registered
extractors; the list and per-kernel status are in
[backends/hip/overview.md](../backends/hip/overview.md). With
`enable_hip=false` the public HIP entry points are stubs that return `-ENOSYS`.

`enable_hipcc=true` additionally compiles the device kernels with `hipcc` and
embeds the resulting HSACO fat binary. Without it the extractors that have a
device kernel return `-ENOSYS`, so any real on-device dispatch needs
`enable_hipcc=true`. Setting `enable_hipcc=true` with `enable_hip=false` only
prints a configure warning.

ROCm 7 or newer is required; `gfx1036` (RDNA 2) is the tested part. See
[ADR-0212](../adr/0212-hip-backend-scaffold.md) and
[ADR-0373](../adr/0373-hip-batch2-float-motion.md).

### `hip_gfx_targets`

Comma-separated GFX targets for `hipcc --offload-arch`, for example
`gfx1036,gfx1100`. When empty, the build asks `rocm_agent_enumerator`, then
`hipconfig`. If both probes fail (the usual case in a build sandbox without a
GPU) it falls back to `gfx90a,gfx1030,gfx1036,gfx1100`: CDNA2, RDNA2 desktop,
the Raphael iGPU and RDNA3.

Set it when auto-detection misparses, when cross-compiling for a remote GPU,
or to shrink the fat binary to one target.

### `enable_float_vif_hip_autodispatch`

Defines `FLOAT_VIF_HIP_AUTODISPATCH`, which sets
`VMAF_FEATURE_EXTRACTOR_HIP` on the `float_vif_hip` descriptor so the model
registry, `--backend hip` and a VMAFx context on a HIP device pick it for
`float_vif`
([ADR-0623](../adr/0623-scaffold-audit-p2-half-finished.md)).

On by default since [ADR-2092](../adr/2092-vmafx-hip-device-frames.md): the
option was off until HIP device pictures existed (T7-10c), and the HIP lane
of the VMAFx API now hands the twins frames in HIP device memory. With
`-Denable_float_vif_hip_autodispatch=false` the twin runs only when named
(`--feature float_vif_hip`), and a context on a HIP device refuses imported
frames for `float_vif` (admission names the CPU extractor).

### `enable_metal`

`auto` probes for `Metal.framework` and `MetalKit.framework` on macOS and
disables the backend elsewhere
([ADR-0361](../adr/0361-metal-compute-backend.md)). It needs an Apple Silicon
GPU (M1 or newer, GPU Family Apple 7+); on an Intel Mac the backend reports
`-ENODEV` at run time. The runtime, IOSurface import and 17 feature
extractors are live; the SpEED family is the one remaining gap. See
[backends/metal/index.md](../backends/metal/index.md).

### `enable_dnn`

Builds the tiny-AI ONNX Runtime surface. The three values behave differently:

| Value | Behaviour |
| --- | --- |
| `auto` | Tries to link ONNX Runtime and silently disables the surface when it is missing; the DNN tests are skipped without a failure. |
| `enabled` | Fails the configure step when ONNX Runtime is unavailable. Use it in CI that wants tiny-AI coverage. |
| `disabled` | Omits the `dnn.h` symbols entirely. |

See [ADR-0022](../adr/0022-inference-runtime-onnx.md).

### Embedded MCP options

`enable_mcp=true` compiles the embedded MCP (Model Context Protocol) server
into libvmaf and enables the `vmaf_mcp_*` symbols. At run time it serves
`list_features` and `compute_vmaf` over every transport compiled in; tools
that mutate the measurement thread are future work. Usage is in
[`docs/mcp/embedded.md`](../mcp/embedded.md).

The server is POSIX code (Unix-domain and TCP sockets, `<unistd.h>`). On
Windows, `enable_mcp=true` stops configure with an error that says so
([ADR-2646](../adr/2646-posix-only-build-options.md)).

Each transport has its own flag and needs `enable_mcp=true`:

| Flag | Transport | Notes |
| --- | --- | --- |
| `enable_mcp_sse` | SSE over loopback HTTP | Plain POSIX sockets, no third-party HTTP library. |
| `enable_mcp_uds` | Unix-domain socket | POSIX only; other hosts return `-ENODEV` at run time. Socket paths are created with mode 0700. |
| `enable_mcp_stdio` | Standard I/O | Newline-delimited JSON-RPC on a caller-supplied fd pair; LSP `Content-Length:` framing is a possible later addition. |

### `fuzz`

Builds the libFuzzer harnesses under `core/test/fuzz/`
([ADR-0270](../adr/0270-fuzzing-scaffold.md), OSSF Scorecard `Fuzzing`
remediation). It needs `clang`; pair it with `-Db_sanitize=address` for heap
coverage. Opt-in only. The harnesses are POSIX code (`<unistd.h>`): on Windows,
`fuzz=true` stops configure with an error that says so
([ADR-2646](../adr/2646-posix-only-build-options.md)).

### `enable_rust_features`

Builds the Rust extractors ([ADR-1713](../adr/1713-rc4-rust-extractor-framework.md)):
the Rust twins of C extractors, selected at run time with
`VMAF_FEATURE_IMPL=rust`, and the TAD pilot
([ADR-0707](../adr/0707-vmafx-rust-pilot-feature.md)). It needs `cargo` on
`PATH` (no network, no cbindgen); the build runs an offline `cargo build -p
vmafx-core-rs` and links the one archive into `libvmaf`. Without `cargo`,
configure warns and builds only the C extractors. Off by default; the `Rust`
workflow builds it. See
[Rust extractor framework](rust-extractor-framework.md).

## Flag interactions

| Flag | Interaction | Consequence |
| --- | --- | --- |
| `enable_asm=false` | Disables every SIMD path. | A pure-scalar binary, much slower than the AVX2 path; an escape hatch for toolchains that cannot compile the `*.asm` kernels. |
| `-Denable_avx512=true` | Downgraded when `nasm --version` is below 2.14 or the host headers lack the intrinsics. | Scripted builds must not assume the flag survives configure. |
| `enable_cuda` with `enable_sycl` | Both compile into one binary. | The runtime picks one backend per extractor; `--no_cuda` / `--no_sycl` pin a backend for A/B runs. |
| `enable_nvcc=false` | Uses the clang CUDA driver. | Experimental; clang CUDA lags `nvcc` on new toolchains, so use it only to investigate codegen regressions. It cannot compress device code: pair it with `-Dcompress_device_code=false`. |
| `sycl_compiler=acpp` | AdaptiveCpp has no device image compression. | Pair it with `-Dcompress_device_code=false`, or configure stops. |
| `enable_float` | Adds float twins on top of the integer path. | Turning it off never removes an integer extractor. |
| `enable_dnn=auto` | Skips DNN tests when ONNX Runtime fails to link. | The gap is not reported as a failure; use `enabled` in CI. |
| `enable_mcp=true` on Windows | The embedded MCP server is POSIX code (sockets, `<unistd.h>`). | Configure stops with an error that names the dependency ([ADR-2646](../adr/2646-posix-only-build-options.md)). |
| `fuzz=true` on Windows | The libFuzzer harnesses include `<unistd.h>`. | Configure stops with an error that names the dependency. |
| `enable_sycl=true` on Windows | The `vmaf_vpl` tool needs VA-API, libva-drm and `<unistd.h>`. | The tool is not looked for; configure prints `vmaf_vpl tool: disabled (Linux only: ...)`. |

## Standard Meson options that matter

These come from Meson, not from `meson_options.txt`, but they change the
emitted artifact.

| Option | Default | Effect |
| --- | --- | --- |
| `buildtype` | `release` | `debug` / `debugoptimized` / `release` / `minsize` / `plain` (project default from `core/meson.build`; Meson's own default is `debug`). |
| `default_library` | `both` | `shared` / `static` / `both`; `both` builds `libvmaf.so` and `libvmaf.a` and is what the test suite layout uses. |
| `b_ndebug` | `false` | `true` disables C `assert()`; set with `-Db_ndebug=true`. |
| `b_sanitize` | `none` | `address`, `undefined`, `address,undefined`, `thread`, `memory`. |
| `b_lto` | `true` | LTO is on by default; measurable speedup on the scalar and AVX2 paths. Use `-Db_lto=false` for sanitizer and fuzzing builds. |
| `b_lto_threads` | `4` | Per-link LTO partition parallelism (`-flto=4` on GCC, `-flto-jobs=4` on Clang). GCC's plain `-flto` uses every core per link and `ninja -j N` multiplies that by N ([ADR-1172](../adr/1172-bound-lto-link-parallelism.md)); `-Db_lto_threads=0` restores the compiler default on a dedicated build box. |
| `c_args` | empty | Extra C flags. |
| `prefix` | `/usr/local` | Install prefix for `ninja install`. |
| `pkg_config_path` | system | Set it to link a non-system ONNX Runtime for `enable_dnn`. |

!!! note
    The picture-pool allocator is always on
    ([ADR-0104](../adr/0104-picture-pool-always-on.md)); no `c_args` define is
    needed.

## Floating-point contraction is off everywhere

No C or C++ compile command of libvmaf may fuse `a * b + c` on its own
([ADR-1461](../adr/1461-strict-fp-every-translation-unit.md)).
`core/src/meson.build` passes the compiler's strict floating-point policy as a
project argument, so a GCC build and a clang build, and an x86-64 build and an
aarch64 build, round the same expressions the same way. Code that wants a fused
multiply-add calls the intrinsic or `fma()`.

| Compiler | Flags in `vmaf_strict_fp_args` |
| --- | --- |
| GCC, clang | `-ffp-contract=off` |
| icx, icpx (`intel-llvm`) | `-fp-model=precise -fno-fast-math -fcomplex-arithmetic=full -ffp-contract=off` (order matters: precise first, contraction-off last) |
| icx-cl (`intel-llvm-cl`) | `/fp:precise /clang:-fno-fast-math /clang:-fcomplex-arithmetic=full /clang:-ffp-contract=off` |
| MSVC | `/fp:precise` |
| clang-cl | `/clang:-ffp-contract=off` |

Do not undo this from the command line or in a target:
`-Dc_args=-ffp-contract=fast`,
`-ffast-math` or, with icx, a trailing `-fp-model=precise` change scores.
`test_strict_fp_compiler_args` (in the `fast` suite) reads the compile commands
of the build it runs in and fails when a C or C++ command does not end its
floating-point flags on the strict one.

The device compilers (nvcc, hipcc, icpx for SYCL kernels) have their own lists
with the same effect.

### What the policy changed for icx builds

An icx build takes the same argument, which for icx also selects the precise
floating-point model in place of its default fast one. The SVM and model code
(`svm.cpp`, `predict.c`, `model.c`, `libvmaf.c`) used to be compiled with `-O3`
alone, and its predicted `vmaf` moved by up to 7.05e-12 on every frame with a
non-zero score, in most cases onto the GCC build's value.

Of 163 measured frames (the Netflix 576x324 pair at 8 and 10 bits and as
10-bit 4:2:2, both 1080p checkerboard pairs, a 16-bit and a 3840x2160 Big Buck
Bunny clip), the frames whose features are identical in an icx and a GCC build
but whose `vmaf` differs went from 153 to 15. No extractor value moved, and a
GCC build did not move.

### CIEDE2000 squares

One difference between compilers came from a call the compilers treated
differently, and it is gone
([ADR-1467](../adr/1467-ciede-squares-as-products.md)). `ciede.c` squared a
`float` with `powf(x, 2)`: GCC called the C library, clang and icx multiplied.
The source now writes the product, so every compiler and C library (GCC,
clang, icx, MSVC, Apple's clang) computes the same expression.

What that changes for you:

- `ciede2000` from a GCC-built binary moves by up to 2.0e-11 on about a third
  of real frames (65 of 180 measured); `--precision max` is needed to see it.
- clang and icx builds do not move.
- GCC and clang builds agree on all 180 measured frames on x86-64 and on
  aarch64.
- An icx build still differed from a GCC build in `ciede` through Intel's math
  library (130 of 180 identical, at most 9.7e-12) until
  [ADR-1495](../adr/1495-icx-system-libm.md); see the next section.

## icx builds use glibc's math library

An icx build and a GCC build return the same CPU scores on Linux, because the
math functions libvmaf calls (`log10`, `pow`, `powf`, `log2f`, `exp`, `log` and
the others) come from glibc's `libm` in both
([ADR-1495](../adr/1495-icx-system-libm.md)). Nothing is needed on the command
line.

The Intel driver links its own math library, `libimf`, into every link it
runs, and an icx-built `vmaf` carried a static copy that the library's calls
bound to. So `core/src/meson.build` passes `-no-intel-lib=libimf` to every C
and C++ link whose compiler is icx or icpx (the block between the
`BEGIN / END VMAF host math library link policy` markers). Putting `-lm` first
does not work: the driver turns a `-lm` it is given into `-limf -lm`.

The option covers host links only, so SYCL device code keeps the device math
libraries. Windows builds with `icx-cl` are not covered
([state row T-ICX-CL-WINDOWS-HOST-MATH-2026-10-03](../state.md)).

### Check a build

`test_icx_system_libm` (in the `fast` suite) reads `libvmaf.so` and `vmaf` and
fails if they take a math function from `libimf`; on a GCC or clang build it
skips:

```bash
python3 scripts/ci/run_meson_test.py -- -C build test_icx_system_libm
```

By hand:

```bash
readelf -d build/src/libvmaf.so.3 | grep NEEDED       # libm.so.6, no libimf.so
readelf -W --dyn-syms build/src/libvmaf.so.3 | grep ' pow@'   # pow@GLIBC_2.29
LD_TRACE_LOADED_OBJECTS=1 LD_WARN=1 LD_BIND_NOW=1 LD_DEBUG=bindings build/tools/vmaf 2>&1 | grep "symbol \`pow'"
```

The last line must name `libm.so.6` as the target. It relocates the program
and its libraries as `ldd -r` does, without running anything. Running the
program with `LD_BIND_NOW=1` instead crashes a SYCL build on Ubuntu with
`Relink ... libimf.so ... for IFUNC symbol 'cosf'`: the SYCL runtime loads
Intel's `libimf.so`, which binds `cosf` to `libm.so.6` without depending on
it, and glibc then calls libm's IFUNC resolver before libm is relocated.

### Measured effect

Before the change, on the Netflix 576x324 pair, both 1080p checkerboards,
48 frames of Big Buck Bunny at 1280x720 and 200 at 3840x2160, 268 of 13288
values at `--precision max` differed between an icx 2026.0 build and a GCC
build:

- `psnr` and `psnr_hvs` outputs by up to 1.4e-14;
- `ciede2000` by up to 6.3e-12 (191 of the 200 4K frames);
- `integer_adm_scale1` by 7.9e-8 (1280x720 frame 26);
- `float_adm` with `adm_f1s3=2.25:adm_f2s0=0.3` by 7.5e-8;
- the default model's `vmaf` by up to 1.3e-7.

Now all 13288 are identical. At the default `%.6f` the old differences were
below the last printed digit except for a few `vmaf` values.

Libraries and binaries built with icx before ADR-1495 still call Intel's
functions, but only in a program that links `libimf` first: the icx-built
`vmaf` did, while a GCC-built `ffmpeg` loading the same library bound it to
glibc. The change cost no time: on a 1920x1080 clip with four threads, `ciede`
went from 95.6 to 54.3 ms per frame in an icx build, and the other extractors
and the default model stayed within 2 % (or within the noise of a busy host for
the model).

For the icx build procedure itself see
[oneapi-install.md](oneapi-install.md).

## How feature flags land in the binary

`core/src/meson.build` reads each option once and stores it in a
`configuration_data()` block that drives:

- the `#define HAVE_AVX512 …` macro baked into the library headers;
- whether `.cu`, `.cpp` and `.asm` source files are added to the target;
- whether the `libvmaf_cuda.h` and `libvmaf_sycl.h` headers are installed;
  they are omitted from the installed tree when their backend is disabled, see
  [core/include/meson.build](../../core/include/meson.build).

## Symbol visibility

Only symbols annotated `VMAF_EXPORT` (defined in
`core/include/libvmaf/macros.h`) appear in the dynamic symbol table of
`libvmaf.so`. Every translation unit is compiled with `-fvisibility=hidden`
(see `core/src/meson.build`): C sources through `vmaf_cflags_common`, C++
sources through `vmaf_cppflags_common`, which every C++ target passes as
`cpp_args`. This stops silent symbol interposition from embedded third-party
code (libsvm, pdjson) and internal helpers.

Downstream consumers that build with `-fvisibility=hidden` need no manual
visibility overrides: every declaration in the installed public headers
carries `VMAF_EXPORT`.

The `check_exported_symbols` test in the `fast` suite runs
[`core/test/check_exported_symbols.py`](../../core/test/check_exported_symbols.py)
against the built `libvmaf.so` on Linux and fails on any export that is not a
`vmaf_*` name declared in a public header:

```bash
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build check_exported_symbols
python3 core/test/check_exported_symbols.py build/src/libvmaf.so.3.0.0 core/include
```

Two kinds of C++ symbol are exempt because they are the runtime's own
definitions: members of namespace `std`, and in SYCL builds members of
namespace `sycl`. Both runtimes declare those namespaces with default
visibility, so a template member a libvmaf TU instantiates (for example the
`std::basic_stringbuf` destructor behind an `std::ostringstream`) is exported
whatever the compile flags.

Sanitizer and debug builds keep more of those template members out of line, so
they export more of them; the check reads demangled names and accepts any of
them. Coverage builds (`-Db_coverage=true`) do not register the test, because
libgcov links into the instrumented library and exports its own globals.

See [ADR-0379](../adr/0379-libvmaf-symbol-visibility.md) and
[Research-0092](../research/0092-round4-symbol-visibility-audit.md) for the
original 207-symbol audit and fix rationale.

## Static linking and `pkg-config`

`core/meson.build` sets `default_library=both`, so a build produces
`libvmaf.so` and `libvmaf.a`, and downstream projects (FFmpeg's
`--enable-libvmaf`, the fork's own `ffmpeg-patches/` stack) discover it through
`pkg-config`.

libvmaf contains C++ translation units: the fork's own converted sources
(`feature_extractor.cpp`, `feature_collector.cpp`, `luminance_tools.cpp`,
`log.cpp`, `read_json_model.cpp`, the C++23 picture pools) and vendored
libsvm. A consumer linking the static archive therefore needs the C++ runtime
on the link line, and `libvmaf.pc` carries it
([ADR-1166](../adr/1166-upstream-issue-harvest.md)):

```console
$ pkg-config --static --libs libvmaf
-L/usr/local/lib -lvmaf -pthread -lm -lstdc++
```

The runtime is chosen from the STL actually in use, not from the compiler id:
`_LIBCPP_VERSION` selects `-lc++`, otherwise `-lstdc++`. That matters because
clang defaults to libstdc++ on Linux and to libc++ on macOS and FreeBSD, and
`-Dcpp_args=-stdlib=libc++` can flip either. MSVC and clang-cl auto-link their
runtime through `#pragma comment(lib)`, so nothing is added there.

To check your own build, link a one-line program against the static flags:

```bash
printf '#include <libvmaf/libvmaf.h>\nint main(void){VmafContext *c=0;VmafConfiguration f={0};return vmaf_init(&c,f);}\n' > /tmp/smoke.c
cc /tmp/smoke.c $(pkg-config --cflags libvmaf) $(pkg-config --static --libs libvmaf) -o /tmp/smoke
```

The `libvmaf-build-matrix` workflow runs exactly that link on its static leg;
grepping the flag list is not sufficient, because the link is what reproduces
the downstream failure.

`-Denable_dnn` static builds additionally carry the ONNX Runtime shared object
in `Libs.private`.

## See also

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md): project-wide
  doc-substance rule (this page satisfies the Build-flag bar).
- [backends/index.md](../backends/index.md): how build flags turn into runtime
  backend availability.
- [getting-started/building-on-windows.md](../getting-started/building-on-windows.md):
  platform-specific toolchain setup.
- [development/release.md](release.md): release build and signing flow.

## History

- **ADR-1166** added the C++ runtime to `Libs.private` of `libvmaf.pc`. Before
  it, `Libs.private` read only `-pthread -lm`, and a static consumer failed
  with several hundred undefined references to `operator new(unsigned long)`
  and `std::ios_base::ios_base()`
  ([Netflix/vmaf#1178](https://github.com/Netflix/vmaf/issues/1178));
  downstream fully-static FFmpeg builds had to add `-lstdc++` by hand (see
  [ADR-0198](../adr/0198-volk-priv-remap-static-archive.md)).
- **September 2026**: the symbol-visibility flags reached every C++ target.
  Until then the C++ sources compiled directly into the library, the C++
  sources of the feature library and the isolated `*_cpp20` / `*_cpp23`
  libraries got none of them, and `libvmaf.so` exported 72 internal symbols,
  among them `aligned_malloc`, `picture_copy` and 64 private `vmaf_*`
  functions. A host application defining any of those names would have
  replaced libvmaf's own.
- **ADR-0726** (2026-05-28) removed the Vulkan backend;
  `subprojects/packagefiles/volk/`
  and `vk-mem-alloc/` remain in the tree pending follow-up cleanup.
- **ADR-0212** (T7-10) added `enable_hip`; **ADR-0104** made the picture pool
  always on, which retired the `-DVMAF_PICTURE_POOL` define.
