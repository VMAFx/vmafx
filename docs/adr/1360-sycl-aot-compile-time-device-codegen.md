<!-- markdownlint-disable MD013 MD060 -->
# ADR-1360: Generate SYCL AOT images at compile time and fail the build when they are missing

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, build, meson, gpu, intel, aot, ci, docker, fork-local

## Context

[ADR-0568](0568-sycl-icpx-aot-targets-default.md) made Intel GPU ahead-of-time (AOT) compilation the default: `core/src/meson.build` compiles every SYCL translation unit with `-fsycl-targets=spir64_gen,spir64 -Xsycl-target-backend=spir64_gen '-device <19 targets>'` and configure prints `SYCL AOT targets (icpx): ...`. The shipped library never contained a native image. `clang-offload-bundler --list` on the installed `libvmaf.so.3` in the dev container prints only `sycl-spir64`.

icpx builds relocatable device code by default. With it, `-fsycl-targets` on a `-c` step only selects which device bitcode goes into the fat object. The device link, `sycl-post-link` and the `ocloc` compile of the `spir64_gen` images all run at the final link, and the link reads its targets from its own command line. Every link in the tree gets its SYCL flags from `sycl_dependency`, whose `link_args` is just `-fsycl` ([ADR-1099](1099-sycl-fsycl-link-propagation.md)), so the link built the default `spir64` target and dropped the `spir64_gen` bitcode without a warning. The option has been inert since ADR-0568 landed. A second gap hid it: icpx needs Intel's `ocloc` offline compiler for `spir64_gen`, the Linux oneAPI compiler does not ship it, and neither the dev container, the production oneAPI builder nor any CI SYCL leg installed it. Any build that did reach the AOT step would have failed.

The cost is a JIT compile of every kernel whenever the Level Zero compiler cache is cold: the first run after an install or a driver update, every run with the cache disabled, and every fresh container or pod. Measured in this change on an Arc B580 and a UHD 770 (see the digest), the default model's first frame took 524 ms on the B580 with a cold cache, 203 ms of which is real work.

## Decision

We will compile each SYCL TU with `-fno-sycl-rdc --offload-compress` in addition to the AOT target flags, so icpx finishes device code generation, ocloc included, at compile time and stores zstd-compressed native images in the object. Every link that pulls the object in keeps them with plain `-fsycl`. `sycl_dependency.link_args` stays `-fsycl`. Configure refuses an AOT build without `ocloc` on `PATH`, and the error names the installer and the JIT-only option. A build step runs `core/src/sycl/check_aot_image.py` on the linked `libvmaf.so` and fails the build unless every ocloc fat binary holds an image for the GFX IP version of every requested target, as `ocloc ids` reports it. The only allowed gap is the Xe2 targets of `integer_psnr_hvs_sycl`: IGC 2.41.5 crashes compiling that kernel for Xe2, so the build compiles it for the other 16 targets and declares the gap to the check. `ocloc` comes from the compute-runtime release pinned as `INTEL_NEO_VERSION` in `build-config.env`, the release the dev container already takes its GPU runtime from. The dev container installs it with that runtime, and `scripts/ci/install-intel-ocloc.sh` installs it on CI runners and in the production oneAPI builder through `dev/scripts/fetch-intel-neo.py --components ocloc`. The default target list stays as ADR-0568 set it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Pass the target and backend flags at every link through `sycl_dependency.link_args` (the first idea) | One place, as ADR-1099 does for `-fsycl`; keeps relocatable device code | Device code generation for the whole library reruns at every link. Relinking only `libvmaf.so` with the 19 targets ran 167 s single-threaded before ocloc aborted, and the 113 test executables that link `libvmaf.a` would each redo it. An ocloc crash in one TU aborts the whole link, and a link-time device list cannot exclude targets for one TU. | Hours of build time and no way around the `psnr_hvs` Xe2 crash |
| `-fno-sycl-rdc` without `--offload-compress` | Simpler section layout | 60 MB of native ISA in `libvmaf.so` (65 MB against 4.6 MB for JIT) and in every test executable; build directory 11 GB instead of 557 MB | Too large for hosted CI runners and release images; compression costs nothing measurable at startup |
| **`-fno-sycl-rdc --offload-compress` at compile time plus a build-time image check (chosen)** | Images exist in every binary that links a SYCL object; the ocloc work runs once per TU and in parallel; per-TU target lists are possible; the check turns a silent regression into a build failure | Forbids `SYCL_EXTERNAL` calls across TUs, which the tree does not use; compile-time flags now decide device codegen, so link-only device flags (for example `-foffload-fp32-prec-div`) must go on the compile line | Chosen |
| Shrink the default target list to cut build time | Faster AOT compile | Drops native images that ADR-0568 promised for every common Intel GPU | Not ours to decide silently; the measured build cost is acceptable |
| Default to JIT (`sycl_icpx_aot_targets=''`) and document the cold start | No ocloc dependency | Reverses ADR-0568, which rejected exactly this as a silent performance trap | Rejected by ADR-0568 |
| Install ocloc from Intel's `noble/unified` apt repository | apt-native | That repository stops at compute-runtime 25.18, too old for the current kernels and for Battlemage ([ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md)); it would pair ocloc with an IGC other than the one the runtime uses | The compute-runtime release is already the pinned, checksum-verified source |

## Consequences

- **Positive**: the default model's first frame on a cold compiler cache drops from 524 ms to 201 ms on an Arc B580 and from 609 ms to 245 ms on a UHD 770; `psnr_sycl` alone from 206 to 123 ms and 129 to 87 ms. With a warm cache AOT and JIT are equal. Scores are bit-identical to the JIT build at `--precision max` on both GPUs for the default model and all 19 SYCL extractors.
- **Positive**: the AOT image check fails the build if a toolchain, flag or rebase change drops the images again, and configure explains a missing ocloc instead of failing in ninja.
- **Negative**: a clean SYCL build at `-j6` on a 22-thread host takes 162 s instead of 82 s; hosted CI runners with 4 cores pay proportionally more. `libvmaf.so` grows from 4.6 MB to 7.7 MB and the build directory from 557 MB to 1.1 GB. Every Linux SYCL build host needs `ocloc` (110 MB with the IGC libraries).
- **Negative**: IGC cannot compile `integer_psnr_hvs_sycl.cpp` for Xe2 (`lnl-m`, `bmg-g21`, `bmg-g31`): ocloc aborts with "longjmp causes uninitialized stack frame". The same fault makes the JIT path segfault on an Arc B580 (`T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29`). That TU carries SPIR-V only on Xe2 until the reworked kernel on `fix/sycl-b580-psnr-hvs-adm-tiny` lands; ocloc compiles that version for all three Xe2 targets, so the `sycl_icpx_aot_igc_skip` entry goes with it. **Amended 2026-09-29:** that kernel rework landed with `fix/sycl-b580-psnr-hvs-adm-tiny`, which empties `sycl_icpx_aot_igc_skip`; every SYCL TU now carries native images for all listed targets.
- **Neutral / follow-ups**: the two self-hosted SYCL jobs (`SYCL Parity (Arc A380)` on `sycl-arc`, `Coverage GPU` on `gpu-full`) configure with `-Dsycl_icpx_aot_targets=` and build the SPIR-V JIT path, because parity and coverage runs need no AOT images and those runners have no ocloc. They need no runner update; a runner that later gains ocloc can drop the override. The Windows MSVC+SYCL leg relies on the ocloc that the Windows oneAPI compiler ships; the image check runs on Linux shared builds only. `INTEL_NEO_VERSION` replaces the `NEO_VER` build argument, and Renovate now tracks it in `build-config.env`.

## References

- Research digest: [Research-2124](../research/2124-sycl-aot-images-dropped-at-link.md).
- [ADR-0568](0568-sycl-icpx-aot-targets-default.md) (AOT by default), [ADR-1099](1099-sycl-fsycl-link-propagation.md) (`-fsycl` at every link), [ADR-1145](1145-neo-stack-derived-from-release.md) (NEO packages derived from one release), [ADR-0541](0541-dev-container-sycl-hip-runtime-fix.md) (why NEO comes from GitHub releases).
- Intel oneAPI DPC++ users manual, `-fsycl-targets` ("normally specified when linking", or with `-c` and `-fno-sycl-rdc`) and `-fsycl-rdc`: <https://intel.github.io/llvm/UsersManual.html>.
- Intel oneAPI ahead-of-time compilation guide (ocloc is packaged with the Windows compiler only): <https://www.intel.com/content/www/us/en/docs/dpcpp-cpp-compiler/developer-guide-reference/2023-1/ahead-of-time-compilation.html>.
- Source: lead brief of 2026-09-29 (RC3 performance bug): "Pass the SYCL target/backend flags at link too", "Provision ocloc wherever icpx SYCL builds run", "Fail closed", "do not silently shrink the default list".
