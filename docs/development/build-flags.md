# Meson build flags

Complete reference for every build-time option libvmaf exposes via
[`core/meson_options.txt`](../../core/meson_options.txt), plus the
standard Meson options that materially change what ships in the binary.

Per [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md), build
flags are a user-discoverable surface and ship documentation in the
same PR as the code.

Pass any option via `-D<name>=<value>` on `meson setup`:

```bash
meson setup build -Denable_cuda=true -Denable_avx512=false -Dbuildtype=release
```

Reconfigure an existing build tree without wiping it:

```bash
meson configure build -Denable_sycl=true
ninja -C build
```

## Project options (`meson_options.txt`)

| Option | Type | Default | Effect |
| --- | --- | --- | --- |
| `enable_tests` | bool | `true` | Build `core/test/` unit tests; `meson test -C build` needs this |
| `enable_docs` | bool | `true` | Build the Doxygen C-API HTML under `build/core/doc/` |
| `enable_tools` | bool | `true` | Build the `vmaf` and `vmaf_bench` CLI binaries |
| `enable_asm` | bool | `true` | Compile any `*.asm` source files (nasm); disables all SIMD paths when `false` |
| `enable_avx512` | bool | `true` | Build the AVX-512 kernels (requires nasm ≥ 2.14); auto-disabled on hosts without AVX-512 headers |
| `built_in_models` | bool | `true` | Compile the default `.json` VMAF models into the library (`version=vmaf_v0.6.1` etc. resolve without disk I/O) |
| `enable_float` | bool | `true` | Compile the `float_*` feature extractors (`float_psnr`, `float_ssim`, `float_ms_ssim`, `float_vif`, `float_adm`, `float_motion`). `float_ansnr` was removed per [ADR-0720](../adr/0720-sunset-float-ansnr.md). On by default so that `--feature float_adm` and related CLI flags work without extra configure flags. |
| `enable_cuda` | bool | `false` | Compile the CUDA backend + `.cu` kernels; requires CUDA toolkit (`nvcc`) |
| `enable_nvtx` | bool | `false` | Instrument CUDA kernels with NVTX ranges for Nsight Systems — see [backends/nvtx/profiling.md](../backends/nvtx/profiling.md) |
| `enable_nvcc` | bool | `true` | Use `nvcc` to compile the CUDA kernel objects (the alternative is the clang CUDA driver); only takes effect when `enable_cuda=true` |
| `enable_sycl` | bool | `false` | Compile the SYCL / oneAPI backend + DPC++ kernels; requires `icpx` on PATH |
| `sycl_compiler` | string | `icpx` | Path or name of the SYCL compiler — only consulted when `enable_sycl=true` |
| `enable_dnn` | feature | `auto` | Build the tiny-AI ONNX Runtime surface. `auto` tries to link ORT and silently disables if it's missing; `enabled` fails the configure step when ORT is unavailable; `disabled` omits the `dnn.h` symbols entirely |
| `enable_vulkan` | feature | `disabled` | **Removed** per [ADR-0726](../adr/0726-drop-vulkan-backend.md) (2026-05-28). The option remains in `meson_options.txt` as a stub that fails configure to surface stale build scripts; the runtime, kernels, and FFmpeg patch shims have all been removed. See [ADR-0860](../adr/0860-ffmpeg-patch-chain-no-op-vulkan-shim.md) for the FFmpeg-patches no-op handling. |
| `enable_mcp` | bool | `false` | Compile the embedded MCP (Model Context Protocol) server inside libvmaf. The runtime serves `list_features` and `compute_vmaf` over stdio, UDS, and loopback SSE when the matching transport flags are enabled; mutating measurement-thread tools remain future v4 work. See [`docs/mcp/embedded.md`](../mcp/embedded.md). |
| `enable_mcp_sse` | feature | `auto` | Compile in the SSE (Server-Sent Events / loopback HTTP) transport for the embedded MCP server. Requires `enable_mcp=true`; implemented with plain POSIX sockets and no third-party HTTP library. |
| `enable_mcp_uds` | bool | `false` | Compile in the Unix-domain-socket transport. Requires `enable_mcp=true`. POSIX-only; non-POSIX hosts return `-ENODEV` at runtime. |
| `enable_mcp_stdio` | bool | `false` | Compile in the stdio transport: newline-delimited JSON-RPC on a caller-supplied fd pair. Requires `enable_mcp=true`; LSP `Content-Length:` framing remains a future compatibility addition. |
| `enable_hip` | bool | `false` | Compile the HIP (AMD ROCm) compute backend. Default off. With `enable_hipcc=false` the public C-API entry points return `-ENOSYS` for unported features; with `enable_hipcc=true` the real kernels are compiled and 8/11 features run on-device (psnr, integer_psnr, float_ansnr, float_motion, float_moment, float_ssim, ciede, integer_motion_v2). Adm/vif/integer_motion remain `-ENOSYS` stubs. ROCm 6+ + `gfx1036` (RDNA 2) tested. See [ADR-0212](../adr/0212-hip-backend-scaffold.md), [ADR-0373](../adr/0373-hip-batch2-float-motion.md), [backends/hip/overview.md](../backends/hip/overview.md). |
| `enable_hipcc` | bool | `false` | Compile real HIP kernels via `hipcc` (vs ENOSYS-stub host TUs only). Required for any real-on-device kernel dispatch. Pair with `enable_hip=true`. |
| `fuzz` | bool | `false` | Build libFuzzer harnesses under `core/test/fuzz/` ([ADR-0270](../adr/0270-fuzzing-scaffold.md), OSSF Scorecard `Fuzzing` remediation). Requires `clang`. Pair with `-Db_sanitize=address` for heap coverage. Default off — opt-in only. |

### Flag interactions

- **`enable_asm=false` disables every SIMD path.** This is an escape hatch for
  toolchains that can't compile the `*.asm` kernels — it gives a pure-scalar
  binary much slower than even the AVX2 path.
- **`enable_avx512` is auto-downgraded.** Even with `-Denable_avx512=true`,
  the build will disable the AVX-512 source files if `nasm --version` is
  < 2.14 or if the host headers don't expose the required intrinsics.
  Scripted builds should not assume this flag survives configure-time.
- **`enable_cuda` + `enable_sycl` can coexist.** Both backends compile into
  the same binary; the runtime picks one per extractor based on which
  backend has a kernel available. Use `--no_cuda` / `--no_sycl` at run
  time to pin a backend for A/B comparisons.
- **`enable_nvcc=false` is experimental.** Clang CUDA support lags `nvcc`
  on newer CUDA toolchains; this flag is intended for maintainers
  investigating codegen regressions, not for day-to-day builds.
- **`enable_float` does not turn off the integer path.** The
  fixed-point extractors always compile; this flag only adds the float
  twins on top. It defaults to `true` so that `--feature float_adm`
  and related CLI flags work out of the box; set `-Denable_float=false`
  only on size-constrained embedded targets that cannot afford the extra
  object files.
- **`enable_dnn=auto` vs `enabled`.** Always set `enabled` in CI if you
  want tiny-AI coverage — `auto` will silently skip the DNN tests if ORT
  fails to link and will not flag the gap as a failure.

### Options referenced in docs but not present

(All build flags referenced elsewhere in the docs are now defined in
`core/meson_options.txt`. `enable_hip` was added by ADR-0212
(T7-10) — the option exists but the backend it enables is a scaffold
returning `-ENOSYS`; see the table above and
[backends/hip/overview.md](../backends/hip/overview.md).)

## Standard Meson options that matter

These come from Meson itself, not `meson_options.txt` — but they change
the emitted artifact, so they belong in the build-time surface.

| Option | Default | Effect |
| --- | --- | --- |
| `buildtype` | `debug` | `debug` / `debugoptimized` / `release` / `minsize` / `plain` |
| `default_library` | `shared` | `shared` / `static` / `both` — `both` is required for the test suite layout |
| `b_ndebug` | `false` | Disable C `assert()` when `true`; set with `-Db_ndebug=true` |
| `b_sanitize` | `none` | `address`, `undefined`, `address,undefined`, `thread`, `memory` |
| `b_lto` | `false` | Enable LTO; measurable speedup on the scalar / AVX2 paths |
| `c_args` | (empty) | Extra C flags. Passing `-DVMAF_PICTURE_POOL` enables the picture-pool allocator (gated test target) |
| `prefix` | `/usr/local` | Install prefix for `ninja install` |
| `pkg_config_path` | (system) | Useful when linking against a non-system ONNX Runtime for `enable_dnn` |

## Recommended configurations

```bash
# Fast iteration (CPU only, debug assertions on, AVX2/AVX-512 kept)
meson setup build \
  -Dbuildtype=debugoptimized \
  -Denable_cuda=false -Denable_sycl=false

# Release with CUDA + NVTX for profiling
meson setup build \
  -Dbuildtype=release \
  -Denable_cuda=true -Denable_nvtx=true \
  -Denable_sycl=false

# CI golden-gate config (matches the Netflix CPU golden job)
meson setup build \
  -Dbuildtype=release \
  -Denable_cuda=false -Denable_sycl=false \
  -Denable_float=true -Dbuilt_in_models=true

# Tiny-AI tests (CI)
meson setup build \
  -Dbuildtype=release \
  -Denable_dnn=enabled

# Sanitizer run (for `make test`)
meson setup build \
  -Dbuildtype=debug \
  -Db_sanitize=address,undefined
```

## How feature flags land in the binary

`core/src/meson.build` reads each option once and stores it in a
`configuration_data()` block that drives:

- the `#define HAVE_AVX512 …` macro baked into the library headers,
- whether `.cu` / `.cpp` / `.asm` source files are added to the target,
- whether the `libvmaf_cuda.h` / `libvmaf_sycl.h` headers are installed
  (these headers are omitted from the installed tree when their backend
  is disabled at build — see
  [core/include/core/meson.build](../../core/include/core/meson.build)).

To inspect the resolved configuration of an existing build tree:

```bash
meson configure build | head -40
```

## Related

## Symbol visibility

All translation units in `libvmaf` are compiled with `-fvisibility=hidden`
(see `core/src/meson.build`). Only symbols explicitly annotated with
`VMAF_EXPORT` (defined in `core/include/libvmaf/macros.h`) appear in
the dynamic symbol table of `libvmaf.so`. This eliminates silent symbol
interposition from embedded third-party code (libsvm, pdjson) and internal
helper symbols.

**For downstream consumers** building their own code with `-fvisibility=hidden`:
the `VMAF_EXPORT` attribute on every declaration in the installed public headers
means you do **not** need to add manual visibility overrides for libvmaf entry
points.

**Verification gate**:

```bash
nm -D --defined-only build/src/libvmaf.so.* | grep ' [TW] ' | grep -v ' vmaf_' | wc -l
# Must print 0
```

See [ADR-0379](../adr/0379-libvmaf-symbol-visibility.md) and
[Research-0092](../research/0092-round4-symbol-visibility-audit.md) for
the original 207-symbol audit and fix rationale.

## See also

- [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md) — project-wide
  doc-substance rule (this page satisfies the Build-flag bar).
- [backends/index.md](../backends/index.md) — how build flags turn into
  runtime backend availability.
- [ADR-0022](../adr/0022-inference-runtime-onnx.md) — `enable_dnn` + ORT
  choice.
- [getting-started/building-on-windows.md](../getting-started/building-on-windows.md)
  — platform-specific toolchain setup.
- [development/release.md](release.md) — release build + signing flow.
