<!-- markdownlint-disable MD029 -->
# Metal (Apple Silicon) compute backend

> **Status: 17 kernels wired, registered, and parity-tested.** The Metal
> backend has a real Apple-Silicon runtime, shared-memory `MTLBuffer`
> picture storage, metallib embedding, and 17 wired feature extractors —
> the full cross-backend metric set: `float_moment_metal`,
> `float_motion_metal`, `float_ms_ssim_metal`, `float_psnr_metal`,
> `float_ssim_metal`, `integer_motion_metal`, `integer_psnr_metal`,
> `motion_v2_metal`, `integer_ssim_metal`, `float_vif_metal`,
> `integer_vif_metal`, `float_adm_metal`, `integer_adm_metal`,
> `integer_ciede_metal`, `integer_psnr_hvs_metal`, `integer_cambi_metal`,
> and `ssimulacra2_metal`. Each carries a per-kernel CPU-vs-Metal score
> parity test (see `core/test/test_metal_kernel_coverage_audit.c`).
> (`float_ansnr_metal` was removed in commit 70ed8b3ce3 / PR #38 together
> with the CPU and HIP twins.)
>
> The dispatch support predicate recognises both those extractor names
> and every key in their provided-features arrays (`psnr_y`, `psnr_cb`,
> The one remaining Metal-twin gap is the SpEED family (`speed_chroma` /
> `speed_temporal`), which has CUDA/SYCL/HIP twins but no Metal kernel yet
> (missing, deferred under `GAP-METAL-MISSING-SPEED-TWINS`). The CUDA twin
> (`core/src/feature/cuda/speed/speed_score.cu`, `speed_chroma_cuda.c`,
> `speed_temporal_cuda.c`) serves as the porting reference (~2,000 LOC total
> estimate: ~350 LOC MSL kernels, ~900 LOC `speed_chroma_metal.mm`, ~750 LOC
> `speed_temporal_metal.mm`).
>
> Governing ADRs:
> [ADR-0361](../../adr/0361-metal-compute-backend.md),
> [ADR-0420](../../adr/0420-metal-backend-runtime-t8-1b.md), and
> [ADR-0421](../../adr/0421-metal-first-kernel-motion-v2.md).

## Why Metal

Apple Silicon (M1+) is the perf story for Apple-platform users. The
fork's existing Apple-Silicon coverage is the NEON SIMD CPU path
(per [ADR-0145](../../adr/0145-motion-v2-neon-bitexact.md) and the
wider NEON twin matrix); this backend adds the GPU compute path that
NEON cannot reach.

Three properties make a native Metal backend worth shipping:

1. **Unified memory.** `MTLBuffer` allocations created with
   `MTLResourceStorageModeShared` are zero-copy across CPU↔GPU; the
   submit-side H2D / D2H staging the CUDA and HIP backends
   spend the bulk of their complexity on collapses to host stores
   and direct `[buffer contents]` reads.
2. **First-party Apple compute API.** OpenCL is deprecated since
   macOS 10.14 (2018) and receives no driver updates; Vulkan reaches
   the GPU only through MoltenVK's translation layer (Vulkan command
   buffer → Metal command buffer rewrite) which adds per-dispatch
   overhead. Metal is the supported user-space surface.
3. **No PCIe boundary.** GPU and CPU share the same DRAM with cache
   coherence; the runtime PR can keep the previous-frame ref Y
   plane in one shared buffer rather than ping-ponging two device
   allocations the way the HIP twin does.

See [ADR-0361 §Context](../../adr/0361-metal-compute-backend.md#context)
for the full reasoning and rejected alternatives (MoltenVK, oneAPI,
OpenCL, Swift-based runtime).

## Apple Silicon only

The runtime PR (T8-1b) gates device selection on
`MTLGPUFamily.Apple7` (M1 and later) via
`-[id<MTLDevice> supportsFamily:]`. Intel Macs and non-macOS hosts
surface as `-ENODEV` from `vmaf_metal_state_init`. Reasoning: Apple
discontinued Intel-Mac GPU parity, and the unified-memory zero-copy
story does not apply on Intel-Mac discrete GPUs (Radeon Pro / Vega)
which sit behind PCIe. See
[ADR-0361 §Apple Silicon-only](../../adr/0361-metal-compute-backend.md#apple-silicon-only-apple-gpu-family-7-reject-intel-mac).

## Build

On macOS:

```bash
meson setup build -Denable_metal=enabled
ninja -C build
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build test_metal_smoke
```

`-Denable_metal=auto` (the default) auto-resolves to enabled on
`host_machine.system() == 'darwin'` and disabled elsewhere.
`-Denable_metal=disabled` suppresses the auto-probe even on macOS.
`-Denable_metal=enabled` forces the Metal frameworks to be linked;
on non-macOS hosts the meson `dependency('Metal')` probe fails the
setup step with a clear missing-framework error.

The backend has zero hard runtime dependencies on non-macOS hosts
because the Metal subdirectory is not entered there unless
`-Denable_metal=enabled` is forced. On macOS the `dependency('Metal')`
/ `dependency('IOSurface')` probes resolve to the system frameworks;
`MetalKit` is optional.

Every kernel (`core/src/feature/metal/*.metal`) is compiled offline with
`-std=metal3.1 -mmacosx-version-min=14.0` (`metal_shader_target_args` in
`core/src/metal/meson.build`, [ADR-1496](../../adr/1496-metal-gate-in-tester-bundle.md)):
the embedded metallib loads on macOS 14 and later. Without the target the
compiler stamps the kernels with the build machine's SDK version, and such a
library refuses to load on an older macOS.

Every kernel also compiles with `-fno-fast-math -ffp-contract=off`
(`metal_shader_strict_fp_args`,
[ADR-1498](../../adr/1498-metal-twins-exact-designs.md)). The Metal
compiler's default is fast math, which may divide through a reciprocal,
reassociate sums and contract `a * b + c` into a fused multiply-add; with fast
math off fp32 `+ - * /`, `sqrt` and `fma` are correctly rounded, and
`-ffp-contract=off` turns off the contraction the safe mode still allows
([Research-1498](../../research/1498-metal-shading-language-fp-semantics.md)).
That is what lets a twin return the CPU's bits: the arithmetic of a ported
twin lives in a header on `core/src/feature/metal/metal_portable.h`, which
compiles both as a kernel include and on the host, where a test holds it
against the CPU extractor value by value.

## Runtime layer

The runtime layer uses Objective-C++ `.mm` TUs under ARC and keeps
Metal object handles opaque at C boundaries as `void *` / `uintptr_t`.
`vmaf_metal_context_new` creates an Apple-Family-7+ `id<MTLDevice>`
and `id<MTLCommandQueue>`, `picture_metal.mm` allocates shared
`MTLBuffer` storage, and `kernel_template.mm` wraps per-feature
command-buffer lifecycle and readback waits.

Kernel sources are Metal Shading Language (`.metal`) compiled to
`.air` and linked into a `default.metallib` with `xcrun metal` /
`xcrun metallib`. The metallib is embedded into the libvmaf binary's
`__TEXT,__metallib` section and loaded by the Obj-C++ host dispatch
files.

## Picture storage and IOSurface import

`picture_metal.mm` allocates shared-storage `MTLBuffer` objects
(`MTLResourceStorageModeShared`), enabling zero-copy memory access between
CPU and Apple Silicon GPU without PCIe staging.

External frames imported via `vmaf_metal_picture_import` (e.g. from VideoToolbox
hardware decoding) are currently handled via `IOSurfaceLock` followed by a
synchronous CPU `memcpy` into the shared-storage `VmafPicture` buffer (ADR-0423).
True zero-copy GPU texture or buffer binding without CPU memcpy
(`[MTLDevice newTextureWithDescriptor:iosurface:plane:]` or direct buffer pointer
mapping with GPU completion/fence tracking) is deferred under
`GAP-METAL-IOSURFACE-NOT-TRUE-ZERO-COPY`.

## Rollout sequence

1. **T8-1 (scaffold PR + batch-1)** — public header, `core/src/metal`
   tree, first consumer registrations, `enable_metal` Meson option,
   smoke test, and macOS CI lane.
2. **T8-1b (runtime PR)** — `MTLCreateSystemDefaultDevice` /
   `id<MTLCommandQueue>` / `id<MTLBuffer>` lifecycle. Runtime entry
   points return `0` on a real Apple Silicon device and `-ENODEV` on
   Intel Mac or non-Apple-Family-7 GPUs.
3. **T8-1c…T8-1j (first kernel batch)** — `motion_v2`, float/integer
   PSNR, float moment, float/integer motion, and float SSIM
   host dispatch + MSL kernels.
4. **T8-2b** — `float_ms_ssim_metal` (ADR-0490): float-precision 5-scale
   MS-SSIM on Metal. Three MSL kernels (`ms_ssim_decimate`, `ms_ssim_horiz`,
   `ms_ssim_vert_lcs`); Wang (2003) weights applied host-side in double
   precision.
5. **T8-2c+** — remaining kernels (VIF, ADM, CIEDE, CAMBI, SSIMULACRA2,
   etc.) follow as their own PRs gated by the `places=4`
   cross-backend-diff lane (per [ADR-0214](../../adr/0214-gpu-parity-ci-gate.md)).
6. **`enable_metal` default flip** from `auto` to `enabled`: only
   after the kernel matrix proves bit-exactness via the `places=4`
   cross-backend gate (mirrors the `enable_hip` roadmap).

## Feature extractor options

### `float_ssim_metal`

`float_ssim_metal` now reaches full option parity with the CPU `float_ssim`
extractor (ADR-0484):

- `enable_lcs` (bool, default `false`) — emit per-frame luminance
  (`float_ssim_l`), contrast (`float_ssim_c`), and structure (`float_ssim_s`)
  sub-scores alongside the composite SSIM score.  When enabled, the
  `float_ssim_vert_combine` kernel accumulates three additional per-WG partial
  sums (L, C, S) in a single threadgroup reduction pass — no extra dispatch.
- `enable_db` (bool, default `false`) — convert the SSIM score to decibels:
  `-10·log10(1 − SSIM)`.  Applied host-side after the partial-sum reduction.
- `clip_db` (bool, default `false`) — clamp the dB output to a finite maximum
  derived from frame dimensions and bit depth.  Mirrors the CPU helper exactly.
- `scale` (int, default `0` = auto-detect) — decimation scale factor.
  v1 supports scale=1 only; `scale=0` on frames where auto-detect would choose
  `scale>1` returns `-EINVAL` at init time with a log message.

Usage example:

```bash
vmaf --feature float_ssim_metal:enable_lcs=true:enable_db=true \
     --reference ref.yuv --distorted dist.yuv ...
```

## CLI usage

The standalone `vmaf` executable supports Metal selection on macOS when
compiled with Metal enabled (gated by `-DHAVE_METAL=1` in `core/tools/meson.build`
per ADR-0422):

- `--backend metal` — exclusive backend selector; engages the Metal device and
  disables sibling backends (defaults device index to 0).
- `--metal_device <N>` — select a specific Metal device by index (default: `0` /
  system default Apple Silicon GPU).
- `--no_metal` — disable Metal dispatch even when built on macOS.

Example:

```bash
vmaf --backend metal \
     --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --model version=vmaf_v0.6.1 \
     --json --output /tmp/output.json
```

### `integer_cambi_metal`

`integer_cambi_metal` supports CAMBI configuration options required by model
configurations (such as the default model `vmaf_v1.0.16_3d0h`):

- `cambi_high_res_speedup` (alias `hrs`, int, default `0`, min `0`, max `2160`)
  — Speed up CAMBI processing by downsampling post-spatial-mask for resolutions
  >= 1080p (possible values: `1080`, `1440`, `2160`, `0`). Matches CPU `cambi`
  and CUDA twins.

## Coordination with NEON

The Metal backend targets the GPU on Apple Silicon. The NEON SIMD
twin matrix (per [ADR-0145](../../adr/0145-motion-v2-neon-bitexact.md))
stays the CPU-side path on the same hardware. The two are
complementary:

- Small / latency-sensitive runs land on NEON via the existing CPU
  dispatch (no GPU command-buffer setup overhead).
- Large / throughput-bound runs land on Metal when one of the shipped
  Metal feature kernels is requested; the GPU's parallelism + unified
  memory eliminate both the
  CPU-bound bottleneck and the H2D / D2H staging cost.

Backend selection follows the standard libvmaf precedence (see
[../index.md](../index.md) §Runtime selection): GPU paths win when
available, CPU SIMD wins otherwise.

## Verification

The macOS CI lane `macOS Metal` in `libvmaf-build-matrix.yml` is where the
backend is verified; it runs on every non-draft PR that touches the C core,
with `-Denable_metal=enabled`, and exercises the smoke test plus the currently
wired kernel batch. `build.yml`'s `macOS Clang+Metal` row also builds and tests
the Metal backend. Neither is a required check, so a red Metal run does not
block a merge ([ADR-1259](../../adr/1259-ci-build-matrix-as-it-runs.md)); check
it before merging Metal changes. Linux-host dev sessions cannot reproduce the
lane locally because `Metal.framework` only exists on macOS hosts.

Reviewers verifying locally on a Mac:

```bash
meson setup build -Denable_metal=enabled
ninja -C build
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
  -C build test_metal_smoke
```

### Measuring the twins against the CPU

The hosted runner has no usable Metal device, so on CI every Metal parity
test skips (exit 77) and no Metal score is ever computed there. A Metal twin
is measured on an Apple device, by the macOS tester bundle
([tester page](../../usage/tester-image.md),
[ADR-1496](../../adr/1496-metal-gate-in-tester-bundle.md)):

- **Parity tests.** `core/test/test_metal_<name>_parity.c`, one per twin
  (the list is `metal_parity_tests` in `core/test/meson.build`), compare the
  twin with its CPU extractor with `==` on every output, ciede at the parity
  gate's `1e-9` math-library bound. They are the CUDA, HIP and SYCL twins'
  cases, through the same shared fixture headers (`float_psnr_twin_parity.h`,
  `ssim_twin_parity.h`, ...): full-range noise at 8 to 16 bits, tiny and odd
  frames, option sets, identical pairs. Each case prints
  `@case <name> pass|fail|skip` and the test goes on after a failure.
- **Option tables.** `test_metal_twin_option_parity` compares every Metal
  twin's option table, provided features and TEMPORAL flag with its CPU
  extractor's, one case per twin.
- **Parity gate.** `scripts/ci/cross_backend_parity_gate.py --backends cpu
  metal --hold-exact metal` compares every gate feature exactly; the bundle
  runs it on its four fixtures. On a Mac with a build:

  ```bash
  python3 scripts/ci/cross_backend_parity_gate.py --vmaf-binary build/tools/vmaf \
    --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --backends cpu metal --hold-exact metal
  ```

- **State rows.** `tools/rc1-tester/image/metal-rows.json` names, per open
  Metal row of [`docs/state.md`](../../state.md), the cases, fixture scores
  and gate cells that close it; the report's `metal_rows` section gives each
  row's verdict.

The same parity tests build on every host as self-tests
(`test_metal_selftest_<name>`, suite `metal-selftest`), with the CPU extractor
standing in for the twin: every `==` case must pass there, so a wrong fixture,
key or option string fails CI before it reaches a tester.

### What a ported twin computes

[ADR-1498](../../adr/1498-metal-twins-exact-designs.md) ports the Metal
twins to the designs that make the CUDA, HIP and SYCL twins return the CPU's
scores bit for bit. Every `.metal` file builds with
`-fno-fast-math -ffp-contract=off`, the arithmetic of each twin lives in a
header that compiles both as Metal Shading Language and as host C, and a host
test holds that header against the CPU extractor value by value. Until a
tester's report shows a twin's parity test passing on an Apple GPU, its state
row stays open.

| Twin | What changed for a user | Host proof |
| --- | --- | --- |
| `float_psnr_metal` | Exact at 10, 12 and 16 bits with large differences: integer sums of the CPU's `float` terms per row segment, rows added in the CPU's order, so frames past 2^53 units match too. | `test_metal_float_psnr_math` |
| `float_moment_metal` | Exact at 16 bits full range (the CPU's `float` squares), and past 2^53 units of the sum (16-bit frames above about 2 megapixels) it forms the CPU's rounded sum with five more kernels; a device whose pipelines cannot run 256 threads per threadgroup fails at init. | `test_metal_float_moment_math`, `test_metal_float_moment_sum` |
| `integer_adm_metal` | Integer decouple reciprocal and gain limit as the CPU computes them. | `test_metal_integer_adm_math` |
| `integer_motion_metal` | Differences frames before the blur, as the CPU; emits `motion_sad_score` and `motion3`; CPU option table (`motion_add_uv` is gone); `motion2` / `motion3` from the CPU's window code. | `test_metal_integer_motion_math` |
| `integer_motion_v2_metal` | Same window code; `motion_fps_weight` and `motion_max_val` applied per frame, as the CPU. | `test_metal_motion_v2_exact_contract.py` |
| `integer_psnr_metal` | Exact 64-bit error sum (the old 32-bit halves lost carries above 2^32); `apsnr` and chroma per pixel format. | `test_metal_integer_psnr_exact_contract.py` |
| `integer_vif_metal` | The CPU's gain integers (one integer division, the CPU's double operations replayed in 64-bit integers when needed). In a model run, frames below 16 pixels go to the CPU `vif`; a direct request on them fails at init. Borders fold as the CPU's mirror. | `test_metal_integer_vif_gain`, `test_metal_integer_vif_math` |
| `integer_cambi_metal` | CPU option table except `heatmaps_path`. | `test_metal_twin_option_tables_contract.py` |
| `float_motion_metal` | Row sums in the CPU's order, `motion3` and the CPU's nine options. | `test_metal_float_motion_math` |
| `integer_ciede_metal` | `ciede.c`'s arithmetic on fp32 pairs, one float per pixel summed in raster order. | `test_metal_ciede_math` |
| `float_adm_metal` | The CPU's arithmetic: CSF weights from the CPU's routine (`adm_f1s0`..`adm_f2s3` are now options), exact fp32 quotient, fp64 expressions as exact pairs, rows added in the CPU's order, the CPU's frame-sum floor. Frames below 17x17 are refused at init. `adm_csf_mode` stays default-only, as on the other GPU backends. | `test_metal_float_adm_math` |
| `float_vif_metal` | The CPU's arithmetic; every `vif_kernelscale` runs (the taps come from the CPU's filter routine), `vif_prescale` uses the CPU's scaler, and the per-scale floors are applied. | `test_metal_float_vif_math` |
| `integer_ssim_metal` | Each pixel's term as the CPU's double term, added in the CPU's raster order. | `test_metal_integer_ssim_math` |
| `float_ssim_metal` | The CPU's window terms with no forced 1 (an identical flat frame gives a finite `enable_db` score, as the CPU), added in raster order. | `test_metal_float_ssim_math` |
| `float_ms_ssim_metal` | The CPU's decimation and window terms, added in raster order per plane and scale, combined as the CPU combines them. | `test_metal_float_ms_ssim_math` |

Options that a twin accepts are now its CPU extractor's, with the same names,
defaults and ranges, so a feature string that works with `--backend cpu` works
with `--backend metal`. Three options are the exceptions.
`integer_cambi_metal` does not declare `heatmaps_path`, because the CPU's
heatmap writer is internal to `cambi.c`. `float_adm_metal` runs
`adm_csf_mode=0` only and marks the option default-only, so a model that asks
for another mode keeps the CPU extractor. `float_ssim_metal` runs at
decimation scale 1 only: a model run whose automatic scale is larger keeps the
CPU extractor, and a direct request for another scale fails at init.

## References

- [ADR-0361](../../adr/0361-metal-compute-backend.md) — original
  audit-first Metal backend ADR.
- [ADR-0212](../../adr/0212-hip-backend-scaffold.md) — HIP scaffold
  precedent (T7-10).
- [ADR-0175](../../adr/0175-vulkan-backend-scaffold.md) — Vulkan
  scaffold precedent (T5-1) — the original audit-first GPU-backend
  pattern.
- [ADR-0145](../../adr/0145-motion-v2-neon-bitexact.md) — motion_v2
  NEON twin on Apple Silicon CPU.
- [ADR-0214](../../adr/0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend gate; the runtime PR's incoming numerics gate.
- [ADR-0490](../../adr/0490-float-ms-ssim-metal-port.md) —
  `float_ms_ssim_metal` port (T8-2b): design rationale for the float
  5-scale MS-SSIM Metal twin.
- Apple Developer documentation — Metal-cpp,
  <https://developer.apple.com/metal/cpp/> (accessed 2026-05-09).
