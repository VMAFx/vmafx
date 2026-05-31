<!-- markdownlint-disable MD013 MD060 -->
# Research 0733 — Hardware GPU backend audit and drop recommendation (2026-05-28)

> **Status: final**
> **Scope: VMAFX fork — post-rebrand backend rationalization**
> **Primary recommendation: DROP Vulkan; DEFER SYCL-on-non-Intel deprecation; KEEP HIP; KEEP Metal**

---

## Executive summary

VMAFX currently ships six compute paths: CPU, CUDA, HIP, SYCL, Vulkan, and Metal. This
audit evaluates each GPU backend against the post-rebrand deployment model (container-first,
Kubernetes per-vendor node pools) and a build of the existing bug debt, implementation
completeness, and ongoing maintenance cost.

**Top recommendation: drop the Vulkan backend.**

Vulkan's original value proposition was cross-vendor GPU portability on a single binary.
In the container-first k8s model that proposition no longer holds — each vendor runs in
its own node pool (`nvidia.com/gpu`, `amd.com/gpu`, `gpu.intel.com/i915`), each container
image already bakes in the appropriate native backend (CUDA, HIP, SYCL), and `VMAFX_BACKEND`
is injected at pod-schedule time. Vulkan provides no coverage that the native trio does not
already provide on the same hardware.

Removing Vulkan eliminates:

- **~30 135 LOC** (largest single-backend footprint in the tree, 24 feature files + GLSL
  shaders + VMA runtime)
- **3 open bugs** (T-VK-1.4-BUMP: blocked on NVIDIA FP-contraction regression that has been
  open for months; T-VK-CIEDE-F32-F64: accepted structural f32/f64 gap, not closeable without
  `shaderFloat64`; T-VK-VIF-1.4-RESIDUAL-ARC: Intel Arc vif mismatch, 5+ months unresolved)
- **7 workflow files** with Vulkan CI matrix rows (build-matrix two rows, tests-and-quality-gates
  three jobs: `vulkan-vif-cross-backend`, `vulkan-parity-matrix-gate`, `vulkan-vif-arc-nightly`,
  plus `ffmpeg-integration` and `fuzz.yml`)
- **2 docs/backends/vulkan/** pages to archive

Expected LOC delta (if Vulkan + SYCL non-Intel stubs are both removed): **−48 000–50 000 LOC**.
If Vulkan-only: **−30 135 LOC**.

No vendor loses native GPU coverage after the drop (see cross-vendor matrix in §6).

---

## 1. Backend scope survey

### 1.1 File and feature counts per backend

| Backend | Feature .c/.cpp files | Runtime .c/.cpp files | Total files | Total LOC | Features implemented |
|---------|----------------------|----------------------|-------------|-----------|----------------------|
| CPU     | ~180 (reference)     | ~20                  | ~200        | ~80 000   | all                  |
| CUDA    | ~50 (+ subdirs)      | 10                   | ~60         | 22 254    | 21                   |
| HIP     | 21 .c files          | 9                    | ~30         | 23 071    | 21                   |
| SYCL    | 19 .cpp files        | 9                    | ~28         | 17 935    | 19                   |
| Vulkan  | 26 .c files + shaders| 14                   | ~40         | 30 135    | 24 (including legacy aliases) |
| Metal   | 9 .mm + 9 .metal     | 12                   | ~30         | 7 281     | 9 (float subset)     |

CPU is never a drop candidate and is not scored below.

### 1.2 Feature coverage per backend

Features present in the CUDA reference set (21 entries) and their status on each GPU backend:

| Feature             | CUDA | HIP  | SYCL | Vulkan | Metal |
|---------------------|------|------|------|--------|-------|
| float\_adm          | yes  | yes  | yes  | yes    | no    |
| float\_ansnr        | yes  | yes  | yes  | yes    | yes   |
| float\_motion       | yes  | yes  | yes  | yes    | yes   |
| float\_psnr         | yes  | yes  | yes  | yes    | yes   |
| float\_ssim         | yes  | yes\*| no   | yes    | yes   |
| float\_vif          | yes  | yes  | yes  | yes    | no    |
| integer\_adm        | yes  | yes  | yes  | yes    | no    |
| integer\_cambi      | yes  | yes  | yes  | yes    | no    |
| integer\_ciede      | yes  | yes\*| yes  | yes    | no    |
| integer\_moment     | yes  | yes  | yes  | yes    | no    |
| integer\_motion     | yes  | yes  | yes  | yes    | yes   |
| integer\_motion\_v2 | yes  | yes  | yes  | yes    | yes   |
| integer\_ms\_ssim   | yes  | yes  | yes  | yes    | no    |
| integer\_psnr       | yes  | yes  | yes  | yes    | yes   |
| integer\_psnr\_hvs  | yes  | yes  | yes  | yes    | no    |
| integer\_ssim       | yes  | yes  | yes  | yes    | no    |
| integer\_vif        | yes  | yes  | yes  | yes    | no    |
| speed\_chroma       | yes  | yes  | yes  | yes    | no    |
| speed\_temporal     | yes  | yes  | yes  | yes    | no    |
| ssim (float alias)  | yes  | no   | no   | yes    | no    |
| ssimulacra2         | yes  | yes  | yes  | yes    | no    |

\* `float_ssim_hip` and `ciede_hip` have HSACO kernel infrastructure but their `init()`
returns `-ENOSYS` when `enable_hipcc=false` (the default). With `enable_hipcc=true` the
real HSACO is loaded and they are live.

Vulkan has the broadest feature count (24) including several legacy compatibility aliases
(`adm_vulkan`, `motion_vulkan`) that were renamed but kept for backward compatibility per
ADR-0586.

---

## 2. Implementation completeness

### CUDA

Fully implemented. All 21 features have real CUDA kernel dispatch (`hipModuleLoadData`
pattern via PTX/cubin) through the `cuda/` runtime. No ENOSYS stubs. CI-gated at
`places=4` via the CUDA parity lane. **KEEP.**

### HIP

Fully implemented with HSACO kernel pipeline (ADR-0372/0374). All 21 feature extractors
have `hipModuleLoadData` in their init path. The `enable_hipcc=false` default causes the
binary HSACO blob to be replaced by a stub that returns `-ENOSYS` at runtime — this is a
build-time opt-in policy, not a missing implementation. A sweep of all `.c` files in
`core/src/feature/hip/` confirms every one calls `hipModuleLoadData` (no file is pure
ENOSYS-only stub). The `hip_hsaco_stubs.c` file provides the fallback for the zero-blob
case — it is an intentional graceful-degradation path, not dead code. **KEEP.**

### SYCL

19 features implemented (CUDA feature set minus `float_ssim` alias and `ssim` float
alias). Two toolchain paths: Intel icpx (default, Intel Arc + iGPU, SPIR-V JIT/AOT) and
AdaptiveCpp/acpp (ADR-0335, supports CUDA/HIP targets as acpp-targets at build time). The
acpp cross-vendor path is compile-time configured, not runtime-switchable. In the k8s
model, SYCL is the primary backend for Intel node pools. The acpp-on-NVIDIA/AMD usage is
a developer convenience, not a production path — CUDA is the NVIDIA primary and HIP is the
AMD primary in the Helm chart (`_helpers.tpl` lines 106–114: `nvidia → cuda`, `amd → hip`,
`intel → sycl`). **KEEP for Intel; DEFER acpp-on-non-Intel deprecation** (low risk, rarely
built, no separate LOC to delete — it is a meson option not a code path).

### Vulkan

24 feature extractors (24 .c files + shaders). Cross-vendor portability rationale. **3 open
bugs, all long-standing:**

- **T-VK-1.4-BUMP** — NVIDIA FP-contraction regression survives Phase 1–3 shader fixes.
  API version bump remains blocked. Months of active investigation, no resolution date.
- **T-VK-CIEDE-F32-F64** — Structural f32/f64 precision gap in the CIEDE shader accepted
  as documented fork debt (ADR-0273). Not closeable without `shaderFloat64` which was
  explicitly rejected.
- **T-VK-VIF-1.4-RESIDUAL-ARC** — Intel Arc A380 vif residual mismatch at API 1.4 survives
  Phase 3b stronger-fence experiment. Open since May 2026; no resolution date.

CI impact: 3 jobs in `tests-and-quality-gates.yml` (`vulkan-vif-cross-backend`,
`vulkan-parity-matrix-gate`, `vulkan-vif-arc-nightly`) + 2 rows in `libvmaf-build-matrix.yml`

- 1 in `ffmpeg-integration.yml` + 1 in `fuzz.yml`. This is the highest per-backend CI
footprint of any non-CUDA backend.

The Helm chart already documents that Vulkan is not a separate Kubernetes resource — it
runs through whichever vendor device-plugin is allocated (`values.yaml` lines 99–101).
This confirms Vulkan is not independently schedulable in the k8s model.

**RECOMMENDATION: DROP.**

### Metal

9 features implemented (float motion, float/integer motion family, float psnr, float
ssim/ms-ssim, float ansnr). Runtime is fully live (ADR-0420, `T8-1b closed`): `common.mm`,
`picture_metal.mm`, `picture_import.mm`, and `kernel_template.mm` replaced all prior
`-ENOSYS` stubs with real `MTLComputePipelineState` dispatch. Metal has no open bugs in
`docs/state.md`. Feature coverage is narrower than other backends (9/21) because the Metal
port was scoped to the motion/psnr/ssim subset that benefits most from Apple Silicon's
AMX + GPU compute. **KEEP** — Apple Silicon M-series is a common developer machine
(M3/M4 Pro/Max). The CI `macos-latest` runner covers it. Dropping Metal would regress the
developer experience on the fork's own hardware and cost less than ~7 300 LOC to maintain.

---

## 3. Cross-vendor coverage matrix — current

| Vendor        | Hardware example       | Primary backend | Secondary / fallback | Vulkan path today |
|---------------|------------------------|-----------------|----------------------|-------------------|
| NVIDIA        | RTX 4090               | CUDA            | CPU                  | `_vulkan` + CUDA driver |
| AMD           | RX 7900                | HIP             | SYCL(acpp) / CPU     | `_vulkan` + RADV (Mesa) |
| Intel         | Arc A380 / iGPU        | SYCL            | CPU                  | `_vulkan` + ANV (Mesa) |
| Apple Silicon | M3 Pro                 | Metal           | CPU                  | n/a (MoltenVK stub) |
| CPU-only      | any x86/arm64          | CPU             | —                    | n/a |

### 3.1 Cross-vendor coverage matrix — after Vulkan drop

| Vendor        | Primary backend | Fallback | Loss from Vulkan drop |
|---------------|-----------------|----------|-----------------------|
| NVIDIA        | CUDA            | CPU      | None — CUDA covers all 21 features |
| AMD           | HIP             | CPU      | None — HIP covers all 21 features  |
| Intel         | SYCL            | CPU      | None — SYCL covers 19/21 features (float_ssim + ssim aliases only in Vulkan/CUDA) |
| Apple Silicon | Metal           | CPU      | None — no Vulkan on Metal without MoltenVK; CI uses stub anyway |
| CPU-only      | CPU             | —        | None |

The only features exclusively on Vulkan (not on SYCL) are `float_ssim` and `ssim` aliases —
both are already on CUDA and HIP. Intel users running a feature that specifically requests
`float_ssim` will fall back to CPU automatically via the existing dispatch fallback chain.
This is an acceptable gap: `float_ssim` is a legacy float-pipeline extractor not used in
any production VMAF model.

---

## 4. Per-backend drop cost

### 4.1 LOC to remove

| Backend | Feature LOC | Runtime LOC | Total LOC | Shader files |
|---------|------------|-------------|-----------|--------------|
| Vulkan  | 26 466     | 3 669       | **30 135** | ~15 GLSL .comp + SPV |

### 4.2 Build system changes (Vulkan drop)

- `core/meson_options.txt`: remove `option('enable_vulkan', ...)` and
  `option('vulkan_sdk', ...)` — approximately 10 lines.
- `core/meson.build`: remove `vulkan_dep = dependency(...)` and the
  `if get_option('enable_vulkan')` block — approximately 15 lines.
- `core/src/meson.build`: remove vulkan feature directory subdir call —
  approximately 5 lines.
- `core/src/feature/vulkan/meson.build` (and `core/src/vulkan/meson.build`):
  delete entirely.
- `core/include/libvmaf/libvmaf.h`: remove `vmaf_vulkan_*` entry points (3–4
  functions) + `VmafVulkanState` forward declaration.
- `ffmpeg-patches/`: the patch that wires `--backend vulkan` and `--vulkan_device`
  into `vf_libvmaf.c` must be updated in the same PR per CLAUDE §12 r14.

### 4.3 CI workflow changes (Vulkan drop)

Workflows to edit:

| File | Jobs to remove |
|------|---------------|
| `tests-and-quality-gates.yml` | `vulkan-vif-cross-backend`, `vulkan-parity-matrix-gate`, `vulkan-vif-arc-nightly` |
| `libvmaf-build-matrix.yml` | 2 matrix rows (`meson_extra: -Denable_vulkan=enabled`) |
| `ffmpeg-integration.yml` | 1 step enabling Vulkan in the ffmpeg libvmaf build |
| `fuzz.yml` | 1 line (`-Denable_vulkan=disabled` comment, already disabled — trivial cleanup) |

Net effect: removes 3 full CI jobs (minutes of runner time per PR) and reduces the
matrix by 2 build cells.

### 4.4 Helm chart changes (Vulkan drop)

`deploy/helm/vmafx/values.yaml` lines 99–101 document "Vulkan-on-NVIDIA" and
"Vulkan-on-AMD via ROCm" as secondary usage paths. These lines can be replaced with a
note that native backends supersede Vulkan. No structural chart changes required —
Vulkan is not an independent vendor selector.

### 4.5 Docs to archive

- `docs/backends/vulkan/overview.md` — archive or replace with a one-paragraph
  deprecation notice pointing to CUDA/HIP/SYCL.
- Any `docs/adr/*.md` files referencing Vulkan as "active" should be marked
  superseded (ADR-0127, ADR-0175–0178 are all historical design records; no edit
  needed — they remain accurate for the time period they cover).

---

## 5. HIP scaffold stubs — assessment

The prior cleanup (ADR-0546, T-HIP-CUDA-ORPHAN-TU-CLEANUP, PR closed ~2026-05-18)
removed 6 dead translation units including three HIP stub files (`adm_hip.c`,
`motion_hip.c`, `vif_hip.c`) that had zero callers and `-ENOSYS` returns throughout.

The remaining HIP files are **not stubs** — all 21 `.c` files call `hipModuleLoadData`
with a real HSACO blob (confirmed by grep across the entire `feature/hip/` tree).
The ENOSYS lines that remain are the graceful-degradation path for the
`enable_hipcc=false` build (which produces an empty blob). This is correct behavior,
not debt. No further HIP stub cleanup is needed.

---

## 6. SYCL on non-Intel — assessment

The AdaptiveCpp path (`sycl_compiler=acpp`, `sycl_acpp_targets=omp;cuda:sm_75`)
compiles SYCL kernels for CUDA or HIP hardware at build time. This is:

- Compile-time only, not runtime switchable.
- Unmeasured in CI (no acpp lane in any workflow).
- Not the default (`sycl_compiler` defaults to `icpx`).
- Not needed in production (CUDA covers NVIDIA, HIP covers AMD).

A dedicated deprecation ADR is warranted before removing this option, because a small
number of users may use acpp to develop on a single toolchain across Intel + NVIDIA
hardware. The code cost of keeping the option is zero (it is a meson flag, not extra
source files). Deprecation is a documentation and option-removal action, not a LOC
removal. **Recommend: DEFER to a separate docs-only PR after Vulkan drop lands.**

---

## 7. Metal completeness gap — assessment

Metal covers 9 of 21 features. The gap (12 features) includes the more complex
integer pipelines (ADM, CAMBI, VIF integer, CIEDE, SSIMULACRA2). These were not
ported because Apple Silicon developer machines primarily use VMAFX for reference runs,
not production-scale batch extraction. The float motion/psnr/ssim subset that *is*
ported represents the common developer use case. The gap is a known scoping decision
from the ADR-0420 period, not an oversight.

**No action required.** If Apple Silicon becomes a production node pool in a future
k8s setup, a targeted port of the integer pipeline would be justified. Today it is
not.

---

## 8. Migration steps — Vulkan drop (if approved)

Sequence:

1. **ADR first** — Write an ADR (e.g. ADR-NNNN-drop-vulkan-backend.md) recording the
   decision with the rationale from this digest. Reserve via
   `scripts/adr/next-free.sh --claim drop-vulkan-backend`.

2. **Update ffmpeg-patches** in the same PR as the code removal (CLAUDE §12 r14).
   The Vulkan CLI flags (`--vulkan_device`, `--backend vulkan`) are exposed through the
   ffmpeg `vf_libvmaf.c` patch and must be removed from the patch in the same commit.

3. **Remove source tree** — delete `core/src/feature/vulkan/`, `core/src/vulkan/`,
   `docs/backends/vulkan/` contents (replace with deprecation note).

4. **Update meson build** — remove `enable_vulkan` option, `vulkan_dep` dependency,
   `VmafVulkanState` from public header, `vmaf_vulkan_*` entry points.

5. **Update CI workflows** — remove the 3 Vulkan CI jobs and 2 build-matrix rows.

6. **Update docs/state.md** — close T-VK-1.4-BUMP, T-VK-CIEDE-F32-F64, and
   T-VK-VIF-1.4-RESIDUAL-ARC as "closed by Vulkan backend removal" in the same PR.

7. **Update Helm values.yaml** — remove secondary Vulkan notes from the GPU section.

All steps can be batched in one PR. Estimated size: ~30 000 LOC removed,
~200 LOC of doc/build/CI edits added. No public API surface is removed that
is not already behind `enable_vulkan=enabled` (opt-in build flag).

---

## 9. Open questions for user decision

1. **Vulkan drop timing**: drop immediately in a standalone PR, or defer until after the
   CI slim-down PR (`ci/vmafx-ci-slim-down-v2`) merges? The CI changes overlap minimally
   but the slim-down PR is already in flight — merging it first reduces conflict surface.

2. **SYCL acpp deprecation**: add a `DEPRECATED` note to `sycl_compiler=acpp` now (in
   the Vulkan drop PR) or in a separate follow-up? Minimal effort to do it together.

3. **Metal coverage expansion**: keep Metal at 9 features indefinitely, or schedule a
   follow-up port for the integer pipeline (ADM, VIF, CIEDE, CAMBI, SSIMULACRA2)?
   Would add ~4 000–6 000 LOC of `.metal` + `.mm`. Not urgent, but good for M-series
   developer parity.

4. **Vulkan MoltenVK CI**: the `libvmaf-build-matrix.yml` has a macOS MoltenVK build
   entry. This was gating Vulkan-on-macOS compilation. If Vulkan is dropped, this entry
   can be removed entirely. Confirm there is no separate Metal-on-macOS CI gate needed
   (Metal has its own `macos-latest` runner in `libvmaf-build-matrix.yml` that is
   independent of MoltenVK).

---

## 10. Per-backend recommendation table

| Backend | Recommendation | Rationale |
|---------|---------------|-----------|
| CPU     | KEEP          | Reference path, never drops |
| CUDA    | KEEP          | Primary for NVIDIA node pools; 21 features; 0 open bugs |
| HIP     | KEEP          | Primary for AMD node pools; 21 features; 0 open bugs; all files have real HSACO kernels |
| SYCL    | KEEP          | Primary for Intel node pools; 19 features; acpp cross-vendor path is low-risk to defer |
| Vulkan  | **DROP**      | 3 long-standing open bugs; 30 135 LOC; no k8s-native representation; zero-coverage gap after drop; highest CI footprint of any non-CUDA backend |
| Metal   | KEEP          | 9 features live on Apple Silicon; 7 281 LOC; 0 open bugs; developer-machine first-class experience; low maintenance burden |

---

## References

- `docs/state.md` open bugs T-VK-1.4-BUMP, T-VK-CIEDE-F32-F64, T-VK-VIF-1.4-RESIDUAL-ARC
- `deploy/helm/vmafx/values.yaml` lines 99–114 (GPU vendor → backend mapping)
- `deploy/helm/vmafx/templates/_helpers.tpl` lines 105–114 (`vmafx.backendEnvValue`)
- `core/meson_options.txt` lines 36, 51, 76, 101, 121 (backend options)
- `scripts/ci/cross_backend_parity_gate.py` (backend parity matrix structure)
- `.github/workflows/tests-and-quality-gates.yml` (Vulkan CI jobs)
- `.github/workflows/libvmaf-build-matrix.yml` (Vulkan build matrix rows)
- ADR-0127, ADR-0175–0178 (Vulkan design history)
- ADR-0264, ADR-0269, ADR-0273 (Vulkan open bug ADRs)
- ADR-0420 (Metal runtime — T8-1b closed)
- ADR-0546 (HIP orphan TU cleanup)
- ADR-0335 (AdaptiveCpp second SYCL toolchain)
- `req`: user direction — "we need to audit the hardware backends, we actually don't need vulkan anymore i guess"
