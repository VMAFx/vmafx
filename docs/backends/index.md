<!-- markdownlint-disable MD013 MD060 -->
# Backends

Pick a backend by hardware, build libvmaf with its option, and select it at
run time with `--backend`. The CPU path (scalar C plus SIMD) is always
built; GPU backends are opt-in at build time, except Metal, which `auto`
probes on macOS.

## Choose a backend

Run every `meson setup` line from the repository root (the Meson source
directory is `core/`). The Run-time column lists the flags of the `vmaf` CLI;
see the [CLI reference](../usage/cli.md#backend-selection).

| Hardware | Backend | Build option | Run-time selection | SDK | Status | Guide |
|----------|---------|--------------|--------------------|-----|--------|-------|
| x86-64 CPU | AVX2 / AVX-512 SIMD | on by default (`enable_asm`, `enable_avx512`) | `--backend cpu`, `--cpumask` | `nasm` 2.14+ for AVX-512 | CPU reference; SIMD kernels are checked against the scalar reference | [x86 SIMD](x86/avx512.md) |
| aarch64 CPU | NEON / SVE2 | on by default (`enable_asm`) | `--backend cpu`, `--cpumask` | none | scalar bits for the contracted features | [ARM](arm/overview.md) |
| NVIDIA GPU | CUDA | `meson setup build core -Denable_cuda=true` | `--backend cuda`, `--no_cuda` | CUDA toolkit 13.4.2 (`nvcc`) | 24 of 25 gate features bit-identical to the CPU; `ciede` bounded at 1e-9 | [CUDA](cuda/overview.md) |
| Intel GPU (Arc, Xe iGPU, Battlemage) | SYCL | `meson setup build core -Denable_sycl=true` | `--backend sycl`, `--sycl_device N`, `--no_sycl` | oneAPI DPC++ 2026.1 (`icpx`), Level Zero 1.34.0, `ocloc` | 24 of 25 gate features bit-identical; `ciede` bounded at 1e-9 | [SYCL](sycl/overview.md) |
| AMD GPU | HIP | `meson setup build core -Denable_hip=true -Denable_hipcc=true` | `--backend hip`, `--hip_device N`, `--no_hip` | ROCm 10.1.0 (`hipcc`) | 19 registered extractors; 24 of 25 gate features bit-identical; `ciede` bounded at 1e-9 | [HIP](hip/overview.md) |
| Apple Silicon | Metal | `-Denable_metal=auto` (default; probes on macOS) | `--backend metal`, `--metal_device N`, `--no_metal` | Xcode / Metal.framework | 17 registered, parity-tested kernels; SpEED is the one gap; no exactness declared yet | [Metal](metal/index.md) |

Versions come from `build-config.env` (`CUDA_VERSION`, `ONEAPI_VERSION`,
`LEVEL_ZERO_VERSION`, `ROCM_VERSION`). "Gate features" are the 25 features of
the cross-backend parity gate; "bit-identical" is defined under
[Numerical agreement](#numerical-agreement).

!!! note
    HIP and Metal are opt-in at run time: without `--backend hip` /
    `--hip_device` (or the Metal equivalents) the backend is never used, even
    in a binary built with it. The Vulkan backend was removed in
    [ADR-0726](../adr/0726-drop-vulkan-backend.md).

<!-- >>> CHART twin-exactness: generated, do not edit -->
<!-- markdownlint-capture -->
<!-- markdownlint-disable MD013 MD033 -->
<figure class="vx-chart" markdown>

![Status of 25 features on CUDA, SYCL, HIP and Metal: 72 twins exact, 3 within a libm bound, 25 not declared.](../assets/charts/twin-exactness.light.svg#only-light){ .vx-chart__static width="452" height="591" }
![Status of 25 features on CUDA, SYCL, HIP and Metal: 72 twins exact, 3 within a libm bound, 25 not declared.](../assets/charts/twin-exactness.dark.svg#only-dark){ .vx-chart__static width="452" height="591" }

<figcaption markdown>Source: the fragments in `scripts/ci/exact_twins.d/` and `LIBM_TWINS` in `scripts/ci/cross_backend_calibration.py`. An exact twin returns the CPU extractor's bits (=) and is compared with tolerance 0; a libm-bound twin differs only through the math library, within the stated bound (≤); a dash means no twin of that backend is declared either way.</figcaption>

</figure>

<details class="vx-chart__table" markdown>
<summary>Data table</summary>

| Feature | CUDA | SYCL | HIP | Metal |
| --- | --- | --- | --- | --- |
| `adm` | exact | exact | exact | not declared |
| `cambi` | exact | exact | exact | not declared |
| `ciede` | libm bound (≤ 1e-09) | libm bound (≤ 1e-09) | libm bound (≤ 1e-09) | not declared |
| `float_adm` | exact | exact | exact | not declared |
| `float_moment` | exact | exact | exact | not declared |
| `float_motion` | exact | exact | exact | not declared |
| `float_ms_ssim` | exact | exact | exact | not declared |
| `float_ms_ssim_chroma` | exact | exact | exact | not declared |
| `float_ms_ssim_lcs` | exact | exact | exact | not declared |
| `float_psnr` | exact | exact | exact | not declared |
| `float_ssim` | exact | exact | exact | not declared |
| `float_ssim_lcs` | exact | exact | exact | not declared |
| `float_vif` | exact | exact | exact | not declared |
| `motion` | exact | exact | exact | not declared |
| `motion_debug` | exact | exact | exact | not declared |
| `motion_mffw` | exact | exact | exact | not declared |
| `motion_v2` | exact | exact | exact | not declared |
| `motion_v2_mffw` | exact | exact | exact | not declared |
| `psnr` | exact | exact | exact | not declared |
| `psnr_hvs` | exact | exact | exact | not declared |
| `speed_chroma` | exact | exact | exact | not declared |
| `speed_temporal` | exact | exact | exact | not declared |
| `ssim` | exact | exact | exact | not declared |
| `ssimulacra2` | exact | exact | exact | not declared |
| `vif` | exact | exact | exact | not declared |

</details>
<!-- markdownlint-restore -->
<!-- <<< CHART twin-exactness -->

## How selection works

At build time the options above decide which backends exist in the binary.
At run time the CLI chooses among those compiled in.

### Auto and explicit selection

- `--backend auto` (the default) soft-falls back: if the priority backend
  fails to initialise, the run silently drops to CPU with a stderr log line.
- `--backend NAME` (`cpu`, `cuda`, `sycl`, `hip`, `metal`) is exclusive. For
  a GPU backend, an init failure is a hard error and the process exits with
  code `100`.
- `--no_cuda`, `--no_sycl`, `--no_hip`, `--no_metal` and `--cpumask` remove
  candidates from the list.

Programmatic selection uses `VmafConfiguration` (`log_level`, `n_threads`,
`n_subsample`, `cpumask`, `gpumask` in
[`libvmaf.h`](../../core/include/libvmaf/libvmaf.h)) plus the per-backend
`vmaf_<backend>_import_state()` calls. Python tooling
(`compat/python-vmaf`, `ExternalProgramCaller`) injects `--backend <name>` into
every child `vmaf` call when the environment variable `VMAF_FORCE_BACKEND` (or
`VMAF_BACKEND`) is set.

### Dispatch precedence

Inside libvmaf, the first rule that applies wins:

```figure
backend-dispatch
```

1. Backends the user disabled (`--no_cuda`, `--no_sycl`, `--no_hip`,
   `--no_metal`, `--cpumask`) are removed from the candidates.
2. If a surviving GPU backend has a twin of the feature, and the twin honours
   the options and the input size, the GPU twin runs.
3. Otherwise the best available CPU SIMD path runs; scalar C is the fallback.

The `--feature` name matters. A name is used exactly as given in `auto` mode:
`--feature ciede` runs the CPU extractor, `--feature ciede_sycl` runs the
SYCL twin. With an explicit device `--backend`, a CPU extractor name runs on
that backend's twin when the twin can honour the options and input size, and
falls back to the CPU with a warning otherwise
([ADR-1359](../adr/1359-cli-feature-backend-twin.md);
[details](../usage/cli.md#feature-extractors-on-a-gpu-backend)).

### Explicit-backend semantics (backend name)

What `--backend NAME` guarantees, per
[ADR-0498](../adr/0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md) and
[ADR-0543](../adr/0543-adr-0498-enforcement-hardening.md):

| Situation | Exit code | stderr | JSON output |
|-----------|-----------|--------|-------------|
| Explicit GPU backend not compiled in or fails to init | `100` (`VMAF_EXIT_BACKEND_INIT_FAILED`) | `vmaf: --backend NAME requested but ...; refusing to silently fall back to CPU (ADR-0498)` | with `--output X.json`, the file is overwritten by a one-line descriptor with keys `error`, `backend_requested`, `errno`, `adr` (always `"ADR-0498"`) and `exit_code` |
| GPU-pinned feature name (`*_cuda`, `*_sycl`, `*_hip`, `*_metal`) whose backend is not active | `100` | same refusal, no silent CPU twin | same descriptor |
| Success | `0` | silent on stdout | gains `backend_used` and `feature_backends` (below) |
| Any other failure | other non-zero codes (for example `101` no frames decoded, `102` input read error, `255` generic) | error text | none |

The `100` code lets a CI gate match `[[ $rc -eq 100 ]]` to tell backend
failures from other errors without parsing stderr. The `vulkan` token was
removed with ADR-0726; passing it is an unsupported-backend error, and a
`_vulkan` feature name has no extractor.

Example:

```bash
# Explicit HIP; errors out hard if no AMD GPU is available.
vmaf --reference ref.yuv --distorted dist.yuv \
     --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
     --model version=vmaf_v0.6.1 --backend hip \
     --json --output /tmp/s.json
# stdout silent on success; /tmp/s.json carries:
#   { ..., "backend_used": "hip" }
# On init failure: exit = 100 (ADR-0543), stderr:
#   vmaf: --backend hip requested but init failed; refusing to
#   silently fall back to CPU (ADR-0498)
# AND /tmp/s.json is overwritten with a structured error descriptor:
#   {"error": "vmaf_hip_state_init failed",
#    "backend_requested": "hip", "errno": -19,
#    "adr": "ADR-0498", "exit_code": 100}
```

### The backend receipt

The JSON output carries a top-level `"backend_used": "NAME"` key echoing what
actually ran (`cpu`, `cuda`, `sycl`, `hip`, `metal`), and a
`"feature_backends"` array listing the backend of every extractor. Since
[ADR-1359](../adr/1359-cli-feature-backend-twin.md) `backend_used` names a
device only when at least one extractor ran on it. It mirrors the MCP-layer
echo added by PR #1251. See
[the CLI receipt](../usage/cli.md#backend-receipt-in-json-output).

## Numerical agreement

"Exact" means: at `--precision max`, the twin returns the same bits as the CPU
extractor on every input, not within a tolerance. Where a twin differs only in
the math library, the gate holds it to a stated bound instead (today `ciede`
at 1e-9, `LIBM_TWINS` in `scripts/ci/cross_backend_calibration.py`).

Where to find the details:

- **Per-twin table:** the generated
  [cross-backend exact twins](../development/cross-backend-exact-twins.md)
  page lists every declared twin. Declarations are one file per twin in
  `scripts/ci/exact_twins.d/<feature>.<backend>`.
- **Gate and tolerances:** the
  [cross-backend gate](../development/cross-backend-gate.md) page gives the
  tolerance table, workflow coverage, failure output and the local sweep
  command. The matrix runner compares CPU against CUDA, SYCL, HIP and Metal.
  CI currently runs CPU to SYCL `float_ssim` on the conditionally enabled Arc
  A380 lane; it is not an all-backend, every-PR matrix. Backend-specific
  tests cover the wider native surface.
- **Parity against Netflix:** the
  [upstream parity guard](../development/upstream-parity.md) checks that
  inherited CPU code returns what Netflix/vmaf returns, bit for bit, except
  for recorded deviations.

GPU results are subject to the gate's tolerances (ADR-0214) and must not be
run against CPU-golden equality assertions.

## Guides

- [RC1 external tester guide](../usage/rc1-tester-guide.md): bounded hardware
  discovery, four-frame CPU-referenced correctness checks, and one shareable
  report across CPU, CUDA, SYCL, HIP and Metal.
- [x86 SIMD (AVX2 / AVX-512)](x86/avx512.md): SIMD optimisation notes.
- [ARM NEON / SVE2](arm/overview.md): aarch64 build, runtime and per-feature
  coverage.
- [CUDA](cuda/overview.md): NVIDIA GPU backend, build and invocation; [CUDA twin
  notes](cuda/twin-notes.md) for per-twin detail.
- [NVTX profiling](nvtx/profiling.md): profiling CUDA with NVIDIA Nsight.
- [SYCL / oneAPI](sycl/overview.md): Intel GPU backend, build and invocation.
- [SYCL bundling](sycl/bundling.md): self-contained deployment without oneAPI.
- [SYCL on Windows](sycl/windows.md): native MSVC and oneAPI build, run and
  test.
- [HIP / AMD ROCm](hip/overview.md): 19 registered feature extractors (the
  full table is on that page). `float_ansnr_hip` was removed in commit
  `70ed8b3ce3` (PR #38).
- [Metal / Apple Silicon](metal/index.md): runtime plus 17 wired, registered,
  parity-tested feature kernels; the SpEED family is the one remaining
  Metal-twin gap.
- [Kernel scaffolding templates](kernel-scaffolding.md) and the
  [context-API contract](context-api-contract.md): for backend contributors.
- [Vulkan](vulkan/overview.md): removed in ADR-0726; historical reference
  only.

Not every feature has every twin. The coverage matrix is in
[feature metrics](../metrics/features.md) and in each per-backend page.

## Parity tests are resolution-sensitive

Several extractors change behaviour with picture size, so a parity test that
pins one fixture cannot see the whole contract. Rules to keep in mind:

- The shared SSIM / MS-SSIM auto-scale is `max(1, round(min(w, h) / 256))`,
  which is always `1` below `min(w, h) = 384`.
- The ADM border crop is `(int)(dim * 0.1 - 0.5)`, which is `0` only for band
  dimensions `<= 14`; a zero crop pulls the first and last row and column into
  the sum.
- Every CUDA parity test also runs as a `*_large` variant against a 960x540
  fixture ([ADR-1206](../adr/1206-gpu-parity-large-fixture-variants.md)),
  which crosses the decimation boundary and is not a multiple of the kernel
  block width. When adding a parity test, add it to the large-fixture list in
  `core/test/meson.build` too.

### float_ssim decimation by backend

| Backend | Decimation on the device | Behaviour at auto scale above 1 |
|---------|--------------------------|---------------------------------|
| CPU | reference | reference |
| CUDA | yes, equal to `--backend cpu` at `--precision max` on every frame measured (576x324, 1920x1080, 3840x2160, 8 to 16 bits; [ADR-1399](../adr/1399-cuda-float-ssim-device-decimation.md), [CUDA guide](cuda/twin-notes.md#float_ssim-runs-on-the-device-at-every-scale-adr-1399)) | runs on the device |
| SYCL | yes, bit for bit ([ADR-1370](../adr/1370-sycl-float-ssim-device-decimation.md), [SYCL guide](sycl/history.md#float_ssim-decimation-on-the-device-2026-09-29)) | runs on the device for 1080p and 4K and every explicit `scale`; falls back only when the reduced picture is smaller than SSIM's 11x11 window |
| HIP | no (scale=1 only) | libvmaf replaces that context with CPU `float_ssim`, keeping its options ([ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md)) |
| Metal | no (scale=1 only) | same CPU replacement ([ADR-1324](../adr/1324-gpu-float-ssim-auto-scale-fallback.md)) |

For automatic model dispatch through the host-picture API, libvmaf evaluates
the first picture before backend initialisation: auto-scale `1` stays on the
selected twin, while a resolved value above `1` replaces only that context
with CPU `float_ssim` and preserves its options (HIP and Metal). This makes
common broadcast dimensions complete without pretending those backends
implement decimation.

Explicitly naming `float_ssim_{hip,metal}` keeps the direct capability
contract: auto at `min(w, h) >= 384` returns `-EINVAL`, and `scale=1` opts
into full-resolution GPU SSIM. Pinning `scale=1` on the GPU while leaving the
CPU on `auto` compares different quantities, not two backends.

The SYCL device-buffer-only `vmaf_read_pictures_sycl()` path has no host
pictures, so `float_ssim_sycl` cannot run there and fails its first frame with
an error.

## Related

- [CLI reference](../usage/cli.md): `--no_cuda` / `--no_sycl` /
  `--sycl_device` / `--cpumask` / `--gpumask` flags.
- [ADR-0022](../adr/0022-inference-runtime-onnx.md): tiny-AI runtime, separate
  from classic VMAF backend dispatch (tiny-AI uses ONNX Runtime execution
  providers).
- [ADR-0027](../adr/0027-non-conservative-image-pins.md): base-image and
  toolchain pins for GPU CI.

## History

### Parity-test gaps that hid three defects

Three separate defects hid behind the resolution gap above: the speed_chroma
4K launch bug
([ADR-1202](../adr/1202-cuda-speed-chroma-4k-launch-bounds.md)), the
float-ADM edge-indexing bug
([ADR-1204](../adr/1204-adm-cm-edge-clamp-gpu-twins.md)) and the
`float_ssim_cuda` scale=1-only limitation, lifted by ADR-1399.

### ADR-0543 explicit-backend hardening

ADR-0543 extended ADR-0498: the explicit-backend exit code became a dedicated
`100` instead of the generic `255` (`int -1` truncated to `uint8_t`), the JSON
error descriptor was added, and a GPU-pinned feature name now fails like an
explicit backend instead of silently registering the CPU twin.
