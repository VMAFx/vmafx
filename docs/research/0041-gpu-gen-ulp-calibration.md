<!-- markdownlint-disable MD060 -->
# Research-0041: GPU-generation ULP calibration corpus design

- **Status**: Active
- **Workstream**: [ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md)
- **Last updated**: 2026-05-01

## Question

What does the training corpus for a per-architecture
`(arch_id, raw_gpu_score) → cpu_score` calibration head need to look
like, and what is the smallest viable proof-of-concept that lets us
decide whether the residual is a true function of `(arch, score)` —
in which case an ONNX head is justified — or whether it collapses to
a per-cell scalar offset, in which case a JSON table is enough
(ADR-0234 alternative (d))?

## Sources

- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) /
  [`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py)
  — per-cell JSON record schema, feature ↔ metric-name mapping,
  per-feature absolute tolerances. The data-collection script reuses
  `FEATURE_METRICS`, `BACKEND_SUFFIX`, and the
  `build_command` / `run_one` / `load_frames` helpers verbatim.
- [ADR-0220](../adr/0220-sycl-fp64-fallback.md) — confirms the SYCL
  fp64-free contract and locates the kernel-side sources of the
  residual (ADM gain limit Q31 split-multiply, VIF fp32 `sycl::fmin`,
  fp32 reductions everywhere).
- Vulkan spec — `VkPhysicalDeviceProperties` (`deviceID`, `vendorID`,
  `deviceName`, `driverVersion`) — the canonical per-device
  identification surface.
- CUDA Runtime API — `cudaGetDeviceProperties` returns
  `cudaDeviceProp` with `name`, `major`, `minor` (compute
  capability), `pciBusID`, `pciDeviceID`. The (major, minor) pair
  maps 1:1 to a generation (e.g. `(8, 9)` → Ada Lovelace,
  `(9, 0)` → Hopper, `(8, 0)` → Ampere A100).
- SYCL 2020 — `device::get_info<info::device::vendor_id>()`,
  `device::get_info<info::device::name>()`,
  `device::get_info<info::device::driver_version>()`. Intel oneAPI
  exposes `info::intel::device::pci_address` as a vendor extension
  for stable per-device identity.
- Mesa lavapipe driver — software Vulkan implementation; reports
  `vendorID = 0x10005` (VK_VENDOR_ID_MESA), deterministic across
  hosts, the canonical "no-hardware" arch in the corpus.

## Findings

### Architectures to cover

The fork targets four backends and roughly seven architecture
families that produce *different* ULP residuals (each is a separate
SPIR-V / PTX / ISA codegen target). The corpus needs at least one
representative per family before the calibration head can claim
generalisation:

| Family | Backend(s) | Detection key | Notes |
|---|---|---|---|
| NVIDIA Ada Lovelace | CUDA, Vulkan | CUDA `major=8, minor=9` / Vulkan `deviceID` 0x2684 (RTX 4090) family | Most likely target hardware |
| NVIDIA Hopper | CUDA | CUDA `major=9, minor=0` | Data-centre; HBM3 |
| NVIDIA Ampere | CUDA, Vulkan | CUDA `major=8, minor=0` (A100) / `8, 6` (RTX 30) | Most-common installed base |
| AMD RDNA2 | Vulkan, HIP (future) | Vulkan `vendorID=0x1002`, `deviceID` in 0x73xx range | RX 6000 series |
| AMD RDNA3 | Vulkan, HIP (future) | Vulkan `vendorID=0x1002`, `deviceID` in 0x744x / 0x747x range | RX 7000 series |
| Intel Battlemage / Alchemist | Vulkan, SYCL | Vulkan `vendorID=0x8086`, `deviceID` in Arc B / A range; SYCL `vendor_id=0x8086` | Arc A380 already in fork CI per ADR-0220 |
| Intel Tiger Lake / Iris Xe | Vulkan, SYCL | Vulkan `vendorID=0x8086`, `deviceID` in Xe-LP range | Mobile / iGPU baseline |
| Mesa lavapipe (software) | Vulkan | Vulkan `vendorID=0x10005` | Hosted-CI baseline; deterministic; the only arch we can collect on without bespoke runners |

This makes seven hardware families plus lavapipe = eight cells of
arch coverage.

### Per-arch detection mechanism (single source of truth)

The data-collection script must record a stable `arch_id` per row.
Proposal: a string of the form `"{backend}:{vendor_id:#06x}:{device_id:#06x}"`
for Vulkan, `"cuda:{major}.{minor}"` for CUDA, and
`"sycl:{vendor_id:#06x}:{driver_version}"` for SYCL. The arch_id
field is opaque to the model — it gets one-hot encoded at training
time — but it must be deterministic and reproducible from the
runtime properties:

- **Vulkan**: `VkPhysicalDeviceProperties.vendorID`,
  `.deviceID`. Read once at device creation and emit on stderr in a
  parseable line (the data-collection script greps it from the
  vmaf binary's debug output, or — if T7-39 plumbs an explicit
  `--print-vulkan-device-info` flag — pulls it from there).
- **CUDA**: `cudaGetDeviceProperties` → `(major, minor)`. The
  fork's CUDA backend already logs the device name at init; we
  can extend the log line to include compute capability without an
  ABI change (it's already in the runtime struct).
- **SYCL**: `device::get_info<info::device::vendor_id>()` and
  `info::device::driver_version`. The driver-version string is
  the most discriminating signal because Intel ships multiple
  Compute-Runtime versions per Arc generation and they produce
  *different* residuals (this is the real lesson from ADR-0220's
  Arc A380 audit — the residual depends on the runtime, not just
  the silicon).

For the proof-of-concept the script emits the arch_id directly from
a `--arch-id` CLI argument that the operator passes in, side-stepping
the runtime-introspection plumbing. The implementation PR will wire
the introspection (and add the parquet column from a runtime
source).

### Training-matrix size

The cross-backend parity gate already iterates `(feature, backend_a,
backend_b)` for every combination. For calibration we only need
`(feature, backend_gpu, frame)` triples — the CPU is the implicit
reference, and pairwise GPU↔GPU is irrelevant for a CPU-equivalent
calibration head.

Counting:

| Axis | Cardinality |
|---|---|
| Features (registered with at least one GPU twin per `FEATURE_METRICS`) | 17 |
| GPU backends per feature | 1–4 (varies; Vulkan has fewest twins today, CUDA has the most) |
| GPU architectures per backend | 1–3 (per the table above; lavapipe is one Vulkan arch) |
| Frames per `(ref, dist)` fixture pair | 100–2400 (Netflix golden short clips → 100–500; long-form fixtures → 2400) |
| Distinct `(ref, dist)` fixture pairs | 3 (Netflix golden) + ~7 (`testdata/`) = ~10 |

Worst-case full corpus: 17 features × ~3 GPU backends × ~3 archs ×
500 frames × 10 pairs ≈ 765 000 rows. At ~120 bytes / parquet row
that is ~90 MiB of corpus per arch — well within tractable.

Wall-clock to populate (lavapipe-only, single host): each `vmaf`
invocation on a Netflix-golden fixture costs ~5 s with lavapipe. The
matrix is `17 features × 1 backend × 1 arch × 10 fixtures = 170`
invocations, so the lavapipe baseline is ~15 minutes of host time —
trivially CI-friendly.

For real hardware the wall-clock scales linearly with the number of
arches; per arch the throughput is much higher than lavapipe (real
GPUs go 50–100× faster), so a full per-arch sweep is **minutes**,
not hours.

### Smallest viable training set (proof-of-concept)

Per the user-direction the smoke target is:

- **1 arch** (lavapipe; deterministic; runs on hosted CI)
- **5 features** (`vif`, `motion`, `psnr`, `float_ssim`, `psnr_hvs`
  — covers integer pipeline + float pipeline + DCT-heavy)
- **100 frames** (Netflix golden `src01_hrc00 / src01_hrc01`
  truncated)

That is 5 × 1 × 100 = 500 rows of labelled data. The `--smoke` mode
of `collect_gpu_calibration_data.py` should produce exactly this
shape and finish in under 5 minutes on a hosted runner.
The collector defaults its replay sidecar to `<output>.manifest.json`;
override with `--manifest-out` when bundling a dated calibration corpus. The
manifest records feature/backend/device selections, source geometry, row count,
and shared AI `run_provenance`.

### Decision criterion (corpus → ONNX-or-JSON-offset)

After the smoke corpus is in hand, fit per-cell linear regressions
of `cpu_score ~ raw_gpu_score`. If the slope is `1.0 ± 1e-6` and
the intercept is constant per cell, the residual is a per-cell
scalar offset and ADR-0234 alternative (d) supersedes the ONNX-head
proposal. If the slope deviates non-trivially or the intercept
varies with `raw_gpu_score`, the ONNX head is justified.

This decision is part of the implementation PR's ADR, not this
research digest.

## Alternatives explored

- **Reuse the parity gate's per-cell `max_abs_diff` field as the
  training signal.** Rejected — `max_abs_diff` is a per-cell scalar
  reduction; the calibration head needs per-frame raw scores. We
  re-run the GPU/CPU pair instead and emit the full per-frame
  metric, mirroring the gate's `diff_frames` shape but recording
  raw values rather than the diff.
- **Train a single architecture-agnostic head over all data with
  arch_id as input.** Considered as the main proposal in
  ADR-0234; the alternative is one head per arch (8 tiny ONNXes
  in the registry instead of 1). The single-head approach with
  one-hot-encoded arch_id is simpler to ship and easier to audit
  but requires retraining whenever we add an arch. Per-arch heads
  let new archs come online incrementally. Decision deferred to
  the implementation PR — the data-collection script is agnostic
  and produces both.
- **Skip the proof-of-concept and target real hardware directly.**
  Rejected on cost grounds: hosted CI (lavapipe) is free; real
  hardware needs a self-hosted runner. We use lavapipe to verify
  the pipeline works end-to-end before asking for hardware time.

## Open questions

- Will the residual on lavapipe be representative of the residual
  on real hardware? Lavapipe is fully deterministic (LLVM-IR
  scalar codegen) and may not exhibit the *same* ULP pattern as
  hardware drivers do. The smoke run will tell us whether the
  lavapipe-trained head transfers usefully or whether we need
  per-hardware corpora from day one.
- Does the residual depend on Vulkan driver version on the same
  silicon? The arch_id schema currently encodes
  `(vendor_id, device_id)` only; SYCL also encodes
  `driver_version`. If empirical evidence shows driver version
  matters for Vulkan too, the schema needs to grow.
- Does pooled vs per-frame calibration give different downstream
  behaviour? The libvmaf score consumers fall into two camps —
  per-frame (CSV/JSON output) and pooled (final VMAF score). The
  calibration head trains on per-frame; the pooled score is then
  re-derived from calibrated per-frame scores. Whether this
  produces the same answer as a separately-trained pooled head
  is an empirical question, deferred to the implementation PR.
- Frame-index dependency? The hypothesis is the residual is
  per-(arch, score-magnitude) only. If it turns out to depend on
  e.g. motion content or scene-change boundaries, the head needs
  more inputs than `(arch_id, raw_score)`.

## Related

- ADRs: [ADR-0234](../adr/0234-gpu-gen-ulp-calibration.md),
  [ADR-0214](../adr/0214-gpu-parity-ci-gate.md),
  [ADR-0220](../adr/0220-sycl-fp64-fallback.md),
  [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md).
- Code: [`ai/scripts/collect_gpu_calibration_data.py`](../../ai/scripts/collect_gpu_calibration_data.py),
  [`scripts/ci/cross_backend_parity_gate.py`](../../scripts/ci/cross_backend_parity_gate.py).
- PRs: this PR.
