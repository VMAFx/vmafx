<!-- markdownlint-disable MD013 MD060 -->
# ADR-1364: Register the SYCL device images of a Windows MSVC build through one explicit device link

- **Status**: Accepted
- **Date**: 2026-09-29
- **Deciders**: lusoris
- **Tags**: sycl, build, meson, windows, msvc, gpu, intel, ci, fork-local

## Context

The `Windows MSVC+SYCL` CI leg only configures and builds ([ADR-0121](0121-windows-gpu-build-only-legs.md)); the runner has no GPU. The first native Windows build that ran on hardware (i9-12900K, Arc B580, UHD 770, oneAPI 2025.1.1, 2026-09-29) failed every SYCL kernel submit with `No kernel named ... was found`: 47 of the 50 tests in the `sycl` suite failed on the B580, and `vmaf --backend sycl` stopped at the first frame.

Meson links MSVC-syntax toolchains (`icx-cl`) with `link.exe` itself; unlike the Linux build, it never runs the link through the `icpx` driver. `link.exe` ignores `-fsycl` (`LNK4044`, printed 275 times per build). A SYCL object is a fat object: its device code sits in `__CLANG_OFFLOAD_BUNDLE__sycl-*` sections, and only the driver's link step unbundles it, wraps each image with `clang-offload-wrapper` and adds the CRT initializer that calls `__sycl_register_lib`. With `-fno-sycl-rdc` ([ADR-1360](1360-sycl-aot-compile-time-device-codegen.md)) the native AOT images are already wrapped objects, but they are still inside a bundle section, and the SPIR-V fallback is wrapped at link time. `link.exe` links neither, so the program's kernel registry was empty.

## Decision

On MSVC-syntax toolchains (`cc.get_argument_syntax() == 'msvc'`, icpx only), `core/src/meson.build` compiles the SYCL translation units as relocatable device code (`-fsycl -fsycl-targets=spir64_gen,spir64`, no device codegen) and runs one explicit device link, `icpx -fsycl -fsycl-link`, over all of them with the AOT device list, `--offload-compress`, `-fp-model=precise` and `-fsycl-max-parallel-link-jobs=8`. Its output is one host COFF object that holds every image and registers them at startup. `core/src/sycl/coff_add_anchor.py` appends the external symbol `vmaf_sycl_device_images` to that object, and `core/src/sycl/common.cpp` asks for it with `#pragma comment(linker, "/include:vmaf_sycl_device_images")`, so any program that pulls the SYCL runtime glue out of `vmaf.lib` also links the registration. `-fsycl` leaves the link arguments there, and `/IGNORE:4078` takes its place, as on the driver's own link line. Every other toolchain keeps the ADR-1360 per-TU code generation unchanged. `vmaf_sycl_registered_kernel_count()` and `test_sycl_kernel_registration` check the registry without a GPU, and the `Windows MSVC+SYCL` leg runs that test.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Explicit device link plus anchor symbol (chosen) | One documented driver mode (`-fsycl-link`); `vmaf.lib` is self-contained for static consumers such as FFmpeg; device-free regression test | Device code for all TUs is generated in one step; a small COFF patcher; `sycl_icpx_aot_igc_skip` cannot leave targets out per TU | — |
| Link executables through the `icx-cl` driver | Same pipeline as Linux | Meson has no driver-link mode for MSVC-syntax compilers (`guess_win_linker` calls `link` / `lld-link` directly); would need a linker wrapper that impersonates `link.exe` | Not expressible in Meson without a fragile impersonation |
| Keep `-fno-sycl-rdc` and run `-fsycl-link` over those objects | Keeps parallel per-TU AOT | `clang-offload-wrapper` crashes (exception `0xE06D7363`) wrapping the `sycl-spir64_gen_image` bundles in oneAPI 2025.1 | Does not work |
| Unbundle each TU's image with `clang-offload-bundler` and reproduce the SPIR-V device link (`llvm-link`, `sycl-post-link`, `llvm-spirv`, `file-table-tform`, `clang-offload-wrapper`) in Meson | No driver mode needed | Couples the build to ten internal driver steps whose flags change between releases | Too fragile |
| Pass the device-link object on every link line through `sycl_dependency.link_args` | No binary patching | Meson cannot order a link after a path it only sees as a string; the installed `vmaf.lib` would still register nothing for downstream static consumers | Leaves external consumers broken |
| `/WHOLEARCHIVE` | Pulls every member | Forces every consumer to link all of `vmaf.lib`; not allowed in a `.drectve` section, so each consumer would have to add it | Pushes the problem onto consumers |
| Build the SYCL backend as a DLL | Driver links the DLL | libvmaf exports no `__declspec(dllexport)` API; MSVC builds are static-only (ADR-0121) | Larger change, same link problem inside the DLL build |

## Consequences

- **Positive**: SYCL runs on native Windows. On the B580 and the UHD 770 all 51 `sycl` suite tests pass, the default model and `vmaf_v0.6.1` agree with the Windows CPU run within the gate, and all 19 SYCL extractors pass the parity gate. A static consumer of `vmaf.lib` gets the registration without extra link flags.
- **Negative**: the device link generates the images of every TU in one build step: 355 s serial, 127 s with 8 parallel link jobs, for 18 targets on an i9-12900K. The anchor patcher depends on the wrapper emitting `.sycl_offloading.descriptor` and a trailing string table; it fails the build with a message when that layout changes. A future `sycl_icpx_aot_igc_skip` entry stops configure on Windows until the device link is split per target group.
- **Neutral / follow-ups**: the Linux `sycl_dependency.link_args = ['-fsycl']` invariant stands; the Windows exception is recorded in `core/src/sycl/AGENTS.md`. Finding this also showed that `scripts/ci/run_meson_test.py` returned 0 on Windows as soon as it started Meson (Windows has no `exec`); it now waits for Meson there, so the Windows MinGW64 and ARM64 MSVC lanes gate their tests again (`T-CI-WINDOWS-MESON-TEST-RUNNER-EXIT-0-2026-09-29`).

## References

- [Research-2125](../research/2125-windows-native-sycl-run.md): measurements, parity tables, timings and the reproduction.
- [ADR-1360](1360-sycl-aot-compile-time-device-codegen.md), [ADR-0568](0568-sycl-icpx-aot-targets-default.md), [ADR-0121](0121-windows-gpu-build-only-legs.md), [ADR-1099](1099-sycl-fsycl-link-propagation.md), [ADR-1333](1333-meson-test-secret-env-sanitization.md).
- Intel oneAPI DPC++ compiler documentation: `-fsycl-link` and `-fsycl-max-parallel-link-jobs`.
- Source: `req` — "CI's `Windows MSVC+SYCL` leg only configures and builds — nothing has ever RUN the Windows SYCL build on a GPU. Do a native Windows build on this host and run it on its GPUs; every defect found is RC3 fix-and-revalidate work."
