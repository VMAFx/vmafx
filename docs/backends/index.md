# Backends

libvmaf supports multiple compute backends for hardware-accelerated quality
assessment. Backends are **opt-in at build time** via meson options and
**selected per-invocation at runtime** through `vmaf` CLI flags or the C API.

| Backend | Meson option | Default-on? | Runtime opt-out | Status |
| --- | --- | --- | --- | --- |
| CPU scalar | always on | yes | n/a | stable |
| x86 AVX2 | auto-detected | yes, when host supports | `--cpumask` | stable |
| x86 AVX-512 | `-Denable_avx512=true` | build-time opt-in | `--cpumask` | stable |
| ARM NEON | auto-detected on aarch64 | yes | `--cpumask` | stable — see [arm/overview.md](arm/overview.md) |
| CUDA | `-Denable_cuda=true` | no | `--no_cuda` | stable — see [cuda/overview.md](cuda/overview.md) |
| SYCL / oneAPI | `-Denable_sycl=true` | no | `--no_sycl` / `--sycl_device N` | stable — see [sycl/overview.md](sycl/overview.md) |
| HIP (AMD) | `-Denable_hip=true` | no | `--no_hip` / `--hip_device N` | `--backend hip` end-to-end working on AMD ROCm hosts ([ADR-0519](../adr/0519-hip-import-state-implementation.md)); 19/22 feature kernels real, 3 legacy stubs (adm, vif, motion). `float_ansnr_hip` was removed in commit 70ed8b3ce3 (PR #38). Dispatch currently routes through CPU twins (HIP scores match CPU bit-exactly). See [hip/overview.md](hip/overview.md). |
| Metal (Apple Silicon) | `-Denable_metal=auto/enabled` | auto on macOS | `--no_metal` / `--metal_device N` | Runtime + 17 wired, registered, parity-tested feature kernels live on Apple Silicon (incl. VIF, ADM, CIEDE, CAMBI, SSIMULACRA2) — see [metal/index.md](metal/index.md); the SpEED family is the one remaining Metal-twin gap |

## Runtime selection

Backend selection in the C engine is controlled via CLI flags on `vmaf`
(e.g. `--backend <name>`, `--no_cuda`, `--no_sycl`; see
[../usage/cli.md](../usage/cli.md) — "Backend selection") or programmatically
through `VmafConfiguration` fields in the C API (`gpu_enable`, `cuda_state`,
`sycl_state`).

In Python tooling and test suites (`compat/python-vmaf`, `ExternalProgramCaller`),
setting the environment variable `VMAF_FORCE_BACKEND=<backend>` (or `VMAF_BACKEND`)
automatically injects `--backend <backend>` into all child `vmaf` invocations.
Note that GPU results are subject to ULP tolerances (ADR-0214) and should not be
run against CPU-golden equality assertions.

Dispatch precedence inside `libvmaf` (highest first):

1. User-disabled backends are removed from the candidate list
   (`--no_cuda` / `--no_sycl` / `--cpumask` ISA bits).
2. If a feature has a GPU kernel and a GPU backend survives the filter, the GPU
   path runs.
3. Otherwise the best available CPU SIMD twin runs; scalar C is the universal
   fallback.

### Explicit-backend semantics (`--backend NAME`)

The `--backend` exclusive selector accepts `auto | cpu | cuda | sycl
| hip | metal`. (The `vulkan` token was accepted prior to ADR-0726;
it now returns exit 1 with an unsupported-backend error.) Per ADR-0498 (2026-05-18):

- `--backend auto` (default) keeps the soft-fallback chain — an init
  failure for the priority backend silently demotes to CPU with a
  stderr log line.
- `--backend NAME` for any *explicit* GPU backend turns init failure
  into a **non-zero exit** with a clear stderr error. CI gates that
  depend on backend-specific scoring no longer silently regress
  when, e.g., a GPU ICD fails to load in a container.
- Per ADR-0543 (extends ADR-0498), the exit code for an explicit-
  backend init failure is a dedicated **`100`** (`VMAF_EXIT_BACKEND_INIT_FAILED`)
  rather than the generic non-zero `255` (`int -1` truncated to
  `uint8_t`). CI gates can match `[[ $rc -eq 100 ]]` to distinguish
  backend failures from other errors without parsing stderr.
- When `--output X.json` is also passed, the libvmaf CLI overwrites
  the output path with a single-line structured JSON descriptor
  carrying `"error"`, `"backend_requested"`, `"errno"`, `"adr"`
  (always `"ADR-0498"`), and `"exit_code"` keys — downstream
  wrappers can decode the failure structurally instead of falling
  back to stderr parsing (ADR-0543).
- Per-feature symmetry (ADR-0543): a feature name ending in
  `_cuda` / `_sycl` / `_hip` / `_metal` is a GPU-pinned
  variant. (The `_vulkan` suffix was retired with ADR-0726 — any
  remaining `_vulkan` feature names are effectively dead code with
  no backing extractor.) If the matching backend isn't active in this
  run (not compiled in, not requested, or failed to init), the CLI
  hard-fails with the same exit `100` + JSON descriptor instead of
  silently registering the CPU twin.
- The JSON output gains a top-level `"backend_used": "NAME"` key
  echoing what actually ran (cpu / cuda / sycl / hip / metal).
  Downstream consumers can confirm dispatch independently of stderr;
  mirrors the MCP-layer echo added by PR #1251.

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

> **Note:** `--backend vulkan` was removed in ADR-0726. Passing it returns
> a non-zero exit with an unsupported-backend error.

Not every feature has every twin — the coverage matrix is in
[../metrics/features.md](../metrics/features.md) per feature and in each
per-backend page below.

## Guides

- [x86 SIMD (AVX2 / AVX-512)](x86/avx512.md) — SIMD optimisation notes
- [ARM NEON](arm/overview.md) — aarch64 backend + build / runtime /
  per-feature coverage
- [CUDA](cuda/overview.md) — NVIDIA GPU backend + build / invocation
- [NVTX profiling](nvtx/profiling.md) — profiling CUDA kernels with NVIDIA Nsight
- [SYCL / oneAPI](sycl/overview.md) — Intel GPU backend + build / invocation
- [SYCL bundling](sycl/bundling.md) — self-contained deployment without oneAPI
  runtime
- [Vulkan](vulkan/overview.md) — **removed in ADR-0726**; historical
  reference only
- [HIP / AMD ROCm](hip/overview.md) — opt-in backend; 19 registered
  feature extractors real (see [hip/overview.md](hip/overview.md) for
  the full table); 3 legacy API stubs (`adm_hip`, `vif_hip`, `motion_hip`)
  are not registered and return `-ENOSYS`. `float_ansnr_hip` was
  removed in commit 70ed8b3ce3 (PR #38).
- [Metal / Apple Silicon](metal/index.md) — auto-on-macOS; runtime +
  17 wired, registered, parity-tested feature kernels live; the SpEED
  family is the one remaining Metal-twin gap

## Cross-backend parity

Every backend pair is gated on every PR by the **GPU-parity matrix
gate** (T6-8 / [ADR-0214](../adr/0214-gpu-parity-ci-gate.md)). The
gate diffs per-frame metrics with a feature-specific absolute
tolerance and emits one JSON / Markdown report per CI run. See
[../development/cross-backend-gate.md](../development/cross-backend-gate.md)
for the tolerance table, how to read failure output, and how to add
a new feature to the matrix.

### The parity tests are resolution-sensitive

Several extractors change behaviour with picture size, so a parity test that
pins one fixture cannot see the whole contract:

- the shared SSIM / MS-SSIM auto-scale is
  `max(1, round(min(w, h) / 256))`, which is always `1` below
  `min(w, h) = 384`;
- the ADM border crop is `(int)(dim * 0.1 - 0.5)`, which is `0` only for band
  dimensions `<= 14`, and a zero crop is what pulls the first and last row and
  column into the sum.

Three separate defects hid behind that gap — the speed_chroma 4K launch bug
([ADR-1202](../adr/1202-cuda-speed-chroma-4k-launch-bounds.md)), the float-ADM
edge-indexing bug
([ADR-1204](../adr/1204-adm-cm-edge-clamp-gpu-twins.md)) and the
`float_ssim_cuda` scale=1-only limitation. Every CUDA parity test therefore
also runs as a `*_large` variant against a 960x540 fixture
([ADR-1206](../adr/1206-gpu-parity-large-fixture-variants.md)), which crosses
the decimation boundary and is not a multiple of the kernel block width. When
adding a parity test, add it to the large-fixture list in
`core/test/meson.build` too.

Note also that `float_ssim_cuda` is a v1 **scale=1-only** extractor: it rejects
any resolution whose auto-detected decimation factor is not 1 — i.e.
`min(w, h) >= 384`, which includes every common broadcast resolution — with
`-EINVAL`. The CPU `float_ssim` has no such limit and decimates instead, so
pinning `scale=1` on the GPU while leaving the CPU on `auto` compares two
different quantities rather than two backends.

## Related

- [../usage/cli.md](../usage/cli.md) — `--no_cuda` / `--no_sycl` /
  `--sycl_device` / `--cpumask` / `--gpumask` flags.
- [ADR-0022](../adr/0022-inference-runtime-onnx.md) — tiny-AI runtime (separate
  from classic VMAF backend dispatch; tiny-AI uses ONNX Runtime execution
  providers).
- [ADR-0027](../adr/0027-non-conservative-image-pins.md) — base-image / toolchain
  pins for GPU CI.
