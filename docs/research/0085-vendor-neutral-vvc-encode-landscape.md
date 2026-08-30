<!-- markdownlint-disable MD013 -->
# Research-0085 — Vendor-neutral VVC GPU encode landscape

- **Status**: Active — most factual claims now verified against primary
  sources. A small number of items remain `[UNVERIFIED]` because they
  require running benchmarks (NN-VC quality lift) or proprietary roadmap
  access (VVenC GPU-port upstream plans). See "Verification status" below.
- **Workstream**: [ADR-0315](../adr/0315-vendor-neutral-vvc-encode-strategy.md),
  [ADR-0314](../adr/0314-vmaf-tune-vulkan-score-quick-win.md) (sibling,
  scoped separately)
- **Last updated**: 2026-05-06

## Verification status — read before citing

This digest was originally drafted (subagent-authored, 2026-05-05) with
several `[verified]` tags that were **not actually verified against the
linked documentation**. It was downgraded to `Status: SKELETON` and every
unverified claim was tagged `[UNVERIFIED]`. The 2026-05-06 revision
(this version) re-runs every open question against primary sources and
flips claims to `[verified]` where a primary source confirms them.

What is verified at write time, with linked source:

| Claim | Source | Verified by |
| --- | --- | --- |
| NVENC supports H.264 / HEVC / AV1 only; **no H.266 encode** in NVIDIA Video Codec SDK 13.0 | <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html> | WebFetch 2026-05-06 (verbatim: "NVENC can perform end-to-end encoding for H.264, HEVC 8-bit, HEVC 10-bit, AV1 8-bit and AV1 10-bit") |
| AMD AMF SDK ships no `VideoEncoderH266.h` / `VideoEncoderVVC.h` header. Latest release v1.5.0 (2025-10-29) lists encoder components: `VideoEncoderVCE.h` (H.264), `VideoEncoderHEVC.h`, `VideoEncoderAV1.h` only | <https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/master/amf/public/include/components> + release v1.5.0 notes | gh-API 2026-05-06 |
| Intel oneVPL public API `mfxstructures.h` defines `MFX_CODEC_VVC`, `MFX_PROFILE_VVC_MAIN10`, `MFX_PROFILE_VVC_MAIN10_STILL_PICTURE`, and 17 VVC level/tier enums. Added in oneVPL 2.15 (2025-04-11): "definitions for VVC Main 10 Still Picture profile and level 6.3" | <https://github.com/oneapi-src/oneVPL/blob/main/api/vpl/mfxstructures.h> + `CHANGELOG.md` 2.15.0 entry | gh-API 2026-05-06 |
| Intel Lunar Lake (Core Ultra 200V, Xe2 iGPU) ships hardware H.266/VVC **decode**; Intel was first chipmaker to ship VVC hardware decode (Sept 2024). Battlemage (Arc B-series, dGPU) does **not** ship VVC decode | Intel Community thread + Phoronix VVC-VA-API-FFmpeg-Decode + multiple secondary outlets | WebSearch 2026-05-06 |
| `VK_KHR_video_encode_h266` does **not exist** in the Khronos registry (404 on `registry.khronos.org`); not even provisional. AV1 encode (`VK_KHR_video_encode_av1`) ratified November 2024 with Vulkan 1.3.302 — sets the precedent for the spec-to-driver lag | <https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_video_encode_av1.html> + Khronos blog post on Vulkan 1.3.302 | curl 2026-05-06 + WebSearch 2026-05-06 |
| Mesa 25.2 (August 2025) merged RADV AV1 Vulkan Video **encode** support; previously RADV had AV1 decode only. Mesa has **no** VVC encode or decode path on RADV / ANV; Intel VVC decode ships via the iHD VA-API media driver, not via Mesa Vulkan | Phoronix "Mesa 25.2 RADV Driver Merges Support For AV1 Vulkan Video Encode" + Mesa release-notes index showing 26.0.x as the May-2026 stable branch | WebSearch + WebFetch 2026-05-06 |
| Fraunhofer HHI VVenC repository has **no** open issues or merged PRs mentioning GPU port (CUDA / HIP / SYCL / OpenCL / Vulkan). Open issues focus on NEON optimisation and CPU-side tuning | <https://github.com/fraunhoferhhi/vvenc/issues> | WebFetch 2026-05-06 |
| ZLUDA is actively developed (release v5, 2025-10-02; 173 total releases); covers cuBLAS, cuFFT, cuDNN, cuSPARSE, CUDA Driver and Runtime APIs. README description: "drop-in replacement for CUDA on non-NVIDIA GPUs". Codec coverage (NVENC / NVDEC) is **not** documented in the README and there is no open-source CUDA VVC encoder to host on top of it | <https://github.com/vosen/ZLUDA> | WebFetch 2026-05-06 |
| Fork's CPU adapter exists at `tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py`; shells out to CPU `vvenc` binary; exposes NN-VC toggles | Fork tree, `vvenc.py` | Direct read |

What remains `[UNVERIFIED]` and why:

| Claim | Why still unverified |
| --- | --- |
| NN-VC quality-lift magnitude (bitrate at iso-VMAF) | Requires running VVenC NN-VC vs. baseline on a representative corpus. No primary-source benchmark publishes a fork-specific number. Item left for a future Tier-1 follow-up. |
| Per-kernel CPU-time distribution of vvenc (motion-estimation / transform / loop-filter %) | Requires `perf` profiling on the fork's `vvenc` binary against a representative corpus. Item is the prerequisite Tier-2 ADR explicitly demands before commitment. |
| Eng-month estimate for HIP-port of vvenc hot kernels | Depends on the profile result above plus an upstream-port-roadmap signal. Both sit in the future-work queue. |

## Question

User (lawrence, 2026-05-05): *"Gotta wonder if you could make CUDA VVC
without nvidia."*

Translated and de-colloquialised: can the fork ship a GPU-accelerated
VVC (H.266) encode path that does **not** require NVIDIA hardware?
Today the fork's only VVC adapter shells out to Fraunhofer HHI's
**CPU** `vvenc` reference encoder. The question is whether a
vendor-neutral GPU encode story exists today, near-term, or only on a
multi-year horizon — and what the cost/value ladder looks like.

## Sources

`[verified]` = directly confirmed against the linked source by a
WebFetch / direct file read at write time. `[UNVERIFIED]` = claim
present but not closeable from public documentation alone.

- `[verified]` Fork-internal:
  - `tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py` — the
    existing CPU adapter, with NN-VC toggle.
  - `tools/vmaf-tune/AGENTS.md` — describes the NN-VC integration
    surface and Phase A scope boundary.
  - [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md) — current
    NVENC adapter ladder (h264 / hevc / av1 only at write time).
  - [ADR-0127](../adr/0127-vulkan-compute-backend.md) — fork's
    Vulkan compute backend (scoring, not encoding).
  - [ADR-0033](../adr/0033-hip-applicability.md) — prior HIP-port
    feasibility note (focused on VMAF features, not codecs).
- `[verified]` NVIDIA Video Codec SDK 13.0 NVENC Application Note
  (<https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html>) —
  NVENC supports H.264, HEVC 8-bit, HEVC 10-bit, AV1 8-bit, AV1 10-bit.
  **No H.266/VVC encode.** Newest codec is AV1, on Ada and Blackwell.
- `[verified]` AMD AMF SDK GitHub repo
  (<https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/master/amf/public/include/components>)
  — encoder components are `VideoEncoderVCE.h` (H.264),
  `VideoEncoderHEVC.h`, `VideoEncoderAV1.h`. **No
  `VideoEncoderH266.h` / `VideoEncoderVVC.h`.** Latest release
  **v1.5.0 (2025-10-29)** does not list any VVC additions.
- `[verified]` Intel oneVPL public API (`api/vpl/mfxstructures.h` on
  `oneapi-src/oneVPL`) — defines `MFX_CODEC_VVC`,
  `MFX_PROFILE_VVC_MAIN10`, `MFX_PROFILE_VVC_MAIN10_STILL_PICTURE`,
  17 VVC level/tier enums. The `CHANGELOG.md` entry for **2.15.0
  (2025-04-11)** explicitly adds "definitions for VVC Main 10 Still
  Picture profile and level 6.3". The **runtime** (`intel/vpl-gpu-rt`)
  exposes VVC **decode** on Lunar Lake silicon. The API surface for
  VVC **encode** is not documented in any oneVPL release notes through
  2.16.0 (2025-12-08).
- `[verified]` Intel Lunar Lake (Core Ultra 200V, Xe2 iGPU) is the
  first shipping CPU/GPU silicon with hardware VVC decode (Sept 2024,
  per Phoronix + flatpanelshd + neowin coverage of Intel's
  announcement). **Battlemage** (Arc B-series discrete GPU) does
  **not** ship VVC decode — Intel community thread confirms.
- `[verified]` Khronos `VK_KHR_video_encode_h266` does **not exist**
  in the registry. `registry.khronos.org` returns 404 for
  `VK_KHR_video_encode_h266`; the Vulkan-Docs GitHub issue tracker has
  zero h266/vvc tickets. AV1 precedent:
  `VK_KHR_video_encode_av1` ratified November 2024 with Vulkan
  1.3.302 — informs the multi-month-to-multi-year spec-then-driver
  lag for any future H.266 ratification.
- `[verified]` Mesa releases (per `docs.mesa3d.org/relnotes.html`):
  - **Mesa 25.0** (April 2025) — incremental AV1 fixes; ANV adds
    initial AV1 decode; no VVC support.
  - **Mesa 25.2** (August 2025) — RADV merges AV1 Vulkan Video
    **encode** support (Phoronix coverage; David Airlie / Red Hat).
  - **Mesa 26.0** (February 2026) — current stable branch (26.0.6 at
    write time); no VVC mention in release-notes index for ANV/RADV.
  - VVC decode on Lunar Lake reaches users via Intel's iHD VA-API
    media driver + FFmpeg, **not** through Mesa Vulkan / RADV / ANV.
- `[verified]` Fraunhofer HHI VVenC GitHub
  (<https://github.com/fraunhoferhhi/vvenc/issues>) — open issues
  list (13 at write time) covers NEON optimisation, encoding tuning,
  multiple-slices support, and library features. **No open or recent
  issue mentions CUDA / HIP / SYCL / OpenCL / Vulkan / GPU port.**
  `[UNVERIFIED]` whether HHI maintains a non-public roadmap.
- `[verified]` ZLUDA project (<https://github.com/vosen/ZLUDA>) —
  active development; release v5 (2025-10-02), 173 releases total.
  Repo top-level dirs: `zluda` (Driver+Runtime API), `zluda_blas`,
  `zluda_blaslt`, `zluda_fft`, `zluda_dnn`, `zluda_dnn8`,
  `zluda_dnn9`, `zluda_sparse`, `dark_api`, `cuda_check`. README:
  "drop-in replacement for CUDA on non-NVIDIA GPUs … near-native
  performance". **No NVENC / NVDEC / codec-workload module** in the
  tree. Even if ZLUDA covered NVENC, no open-source CUDA VVC encoder
  exists to host on it.
- `[verified]` ONNX Runtime EP matrix is documented at
  <https://onnxruntime.ai/docs/execution-providers/> — covers CUDA,
  ROCm, DirectML, OpenVINO, CoreML, WebGPU, QNN, TensorRT, CANN.
  Vendor-neutral inference for NN-VC tools is therefore plausible.

## Findings

### F0. NO vendor ships hardware VVC encode silicon as of 2026-05-06

NVIDIA NVENC SDK 13.0, AMD AMF SDK v1.5.0, and Intel oneVPL 2.16.0
**all** lack VVC encode in their public encoder component lists.
oneVPL ships VVC profile/level enums and `MFX_CODEC_VVC`, but the
runtime (`intel/vpl-gpu-rt`) wires those through to **decode** on
Lunar Lake, not encode. The "vendor-neutral GPU VVC encode" question
therefore has an unusually direct answer today: there is no GPU VVC
encode path on **any** vendor.

### F1. Vulkan-side scoring is shipped today; Vulkan-side encoding is not

The fork has a Vulkan compute backend ([ADR-0127](../adr/0127-vulkan-compute-backend.md))
for VMAF scoring. This is the basis for the sibling
[ADR-0314](../adr/0314-vmaf-tune-vulkan-score-quick-win.md) (separate
PR) which wires `vmaf-tune --score-backend=vulkan`. That gives
non-NVIDIA users vendor-neutral GPU **scoring** of CPU-encoded VVC
bitstreams.

There is no Vulkan-side **encode** path today: `VK_KHR_video_encode_h266`
does not exist in the Khronos registry (verified 404). Even if Khronos
ratified it tomorrow, the AV1 precedent (Nov-2024 ratification → Aug-2025
RADV implementation, ~9-month spec-to-driver lag) suggests realistic
delivery is multi-quarter at minimum after ratification.

### F2. NN-VC is the de-facto vendor-neutral GPU contribution today

VVC's neural-video-coding tools (NN-intra prediction, NN-loop-filter,
NN-super-resolution) run as ONNX models inside the encoder. The
fork's `vvenc.py` adapter exposes them via `--nnvc-intra`,
`--nnvc-loop-filter`, `--nnvc-sr` (verified by reading the file).
ONNX Runtime EP matrix (verified) covers CUDA, ROCm, DirectML,
OpenVINO, CoreML, WebGPU, QNN, TensorRT, CANN — vendor-neutrality of
the *neural portion* is well-supported.

The catch: NN-VC accelerates the **quality** of the encode, not the
**throughput**. The CPU encoder loops still run on CPU; the GPU is
only burning the NN-intra inference. Quality-lift magnitude
remains `[UNVERIFIED]` (no primary-source benchmark on the fork's
representative corpus); the bitrate-at-iso-VMAF claim from the original
draft is parked for a future fork-side benchmark.

### F3. HIP / SYCL port of vvenc hot kernels — feasibility skeleton

The original draft proposed a HIP port of vvenc's motion-estimation,
transform, and loop-filter kernels as Tier 2. The skeleton remains
plausible but per-kernel CPU-time distribution and engineering-month
estimates remain `[UNVERIFIED]`. Confirmed for Tier-2 promotion:

1. A profile run of CPU vvenc on a representative corpus to confirm
   the per-kernel CPU-time distribution. **Not yet done** — Tier-2
   ADR demands this as the prerequisite.
2. Upstream vvenc's GPU-port roadmap: **no public CUDA / HIP / SYCL /
   OpenCL / Vulkan port** in the issue tracker (verified
   `[verified]`). HHI's private roadmap is `[UNVERIFIED]`.
3. Hardware availability for ongoing CI per ADR-0214's GPU-parity
   rule remains an open organisational question.

### F4. ZLUDA is technically interesting but not actionable

ZLUDA could in principle run a (hypothetical) closed-source CUDA
VVC encoder on AMD / Intel hardware. In practice (a) no
open-source CUDA VVC encoder exists, and (b) ZLUDA's verified module
list (`zluda_blas`, `zluda_blaslt`, `zluda_fft`, `zluda_dnn{,8,9}`,
`zluda_sparse`, Driver+Runtime APIs) does **not** include
NVENC/NVDEC. Even an immediate hypothetical CUDA-VVC binary would not
run unless ZLUDA also implemented the NVENC/NVDEC API surface. Not a
near-term path.

## Cost / risk / value matrix

The original draft's matrix included a row "A. NVENC h266 adapter"
that depended on the fabricated NVENC-encode premise; that row is
**removed**. Remaining rows now carry verified vendor-status data:

| Path | Effort | License risk | User value | Verdict |
| --- | --- | --- | --- | --- |
| **Vulkan-scoring quick-win** ([ADR-0314](../adr/0314-vmaf-tune-vulkan-score-quick-win.md), sibling) | Small | None | Non-NVIDIA users get GPU scoring for VVC encodes (encode stays on CPU) | **Tier 1**: ship now via the sibling PR. |
| **NN-VC documentation + corpus integration** | Small | Mixed (NN-VC weights are LGPL-derived; tooling is Apache-2.0) — `[UNVERIFIED]` exact split | Any-GPU users; quality lift magnitude `[UNVERIFIED]` until benchmarked | **Tier 1**: bundle with Vulkan-scoring rollout. |
| **HIP port of vvenc hot kernels** | Medium-large; per-kernel CPU-time distribution `[UNVERIFIED]` until profiled | Apache-2.0 OK; Fraunhofer patent licence still applies | RDNA 3 / 4 / CDNA users; speedup `[UNVERIFIED]` until profile + prototype | **Tier 2**: gated on Tier 1 success and corpus profile. |
| **SYCL port of same kernels** | Incremental over HIP | Apache-2.0 OK | Adds Intel PVC / Xe2 + cross-vendor via Codeplay plugins | **Tier 2.5**: deferred. |
| **Vulkan Video VVC encode adapter** | Driver-side does the work; libvmaf-side small | None | All vendors once silicon + drivers ship | **Tier 3**: revisit quarterly; gated on `VK_KHR_video_encode_h266` ratification (does **not exist** today, verified 404) + at least one driver shipping. |
| **ZLUDA-hosted CUDA-VVC** | n/a | Risky | Zero today (no open-source CUDA VVC encoder; ZLUDA also does not cover NVENC) | **Rejected**. |
| **Wait for AMD/Intel hardware VVC encode** | 0 (passive) | None | None until silicon ships (Lunar Lake = decode-only; Battlemage = no VVC) | Reflected in Tier 3's "revisit". |

## Recommendations (priority order)

### Tier 1 — ship today (small total effort)

1. **Wire vmaf-tune's existing Vulkan scoring** so non-NVIDIA users
   benefit from GPU-accelerated *scoring* of CPU-encoded VVC bitstreams.
   Scoped separately as **ADR-0314** (sibling PR — this digest does
   not implement it).
2. **Document NN-VC as the vendor-neutral GPU contribution today**
   in the user-facing docs. Be explicit that:
   - Hardware VVC encode does **not** exist on any GPU vendor (verified
     for NVIDIA SDK 13.0, AMD AMF v1.5.0, Intel oneVPL 2.16.0).
   - VVenC is the open-source CPU encoder; encoder loops stay on
     CPU but neural tools accelerate on GPU via ONNX Runtime EPs.
   - Vulkan-side scoring closes the GPU loop on the consumption side.

### Tier 2 — backlog, demand-pulled

**HIP port of vvenc's motion-estimation + transform + loop-filter
kernels.** Triggered only when **all three** are true:

- A user has reported CPU-vvenc throughput as a binding constraint
  on a real corpus (not a synthetic benchmark).
- Tier-1 Vulkan-scoring + NN-VC docs have landed and at least one
  downstream consumer is using NN-VC tools through `vmaf-tune` in
  production.
- An RDNA 3 / RDNA 4 or Intel PVC machine is available to the fork
  for ongoing CI (without it the HIP port has no automated
  regression gate and ADR-0214's GPU-parity rule is unenforceable).

If any of the three is false, Tier 2 stays in the backlog. This
matches the fork's "demand-pulled fork-local effort" pattern from
ADR-0009.

### Tier 3 — speculative, revisit quarterly

**Vulkan Video VVC encode adapter.** Triggered by:

- `VK_KHR_video_encode_h266` ratification — verified **does not exist
  today** (Khronos registry 404; zero Vulkan-Docs issues mention it).
- At least one driver shipping the extension once ratified (Mesa RADV,
  NVIDIA proprietary, or ANV — any one suffices to start).
- A libvmaf-side encode adapter design proposal (currently the
  fork's Vulkan code is score-only).

## Reproducer / verification commands

```bash
# 1. Confirm what NVENC actually supports (verified 2026-05-06):
nvidia-smi --query-gpu=name,driver_version --format=csv
ffmpeg -hide_banner -encoders 2>&1 | grep -iE 'nvenc|h266|vvc'
# expected: h264_nvenc / hevc_nvenc / av1_nvenc; no h266_nvenc.

# 2. Confirm AMF / QSV do NOT expose VVC encode (verified 2026-05-06):
ffmpeg -hide_banner -encoders 2>&1 | grep -iE 'h266|vvc'
# expected: only libvvenc enumerates;
# no h266_amf / h266_qsv / h266_nvenc.

# 3. Vulkan-Video extension presence (verified 2026-05-06):
vulkaninfo | grep -iE 'video_(encode|decode)_(h264|h265|av1|h266)'
# expected: h264 / h265 / av1 encode/decode may be present
# on RDNA 3+ (RADV 25.2+) / Battlemage (ANV); h266 absent on all.

# 4. Confirm Khronos registry has no VK_KHR_video_encode_h266:
curl -sIL https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_video_encode_h266.html | head -3
# expected: HTTP 404 (verified 2026-05-06).

# 5. Fork's CPU adapter still works for vendor-neutral encode:
python -m vmaftune encode --codec libvvenc --preset slow \
    --input testdata/yuv/akiyo_qcif.yuv --qp 32 --output /tmp/akiyo.266

# 6. NN-VC tools on whatever GPU is present (ONNX Runtime auto-EP):
python -m vmaftune encode --codec libvvenc --preset medium \
    --nnvc-intra --input testdata/yuv/akiyo_qcif.yuv --qp 32 \
    --output /tmp/akiyo_nnvc.266

# 7. UNVERIFIED-tag count should drop after this revision:
grep -c '\[UNVERIFIED\]' docs/research/0085-vendor-neutral-vvc-encode-landscape.md
# expected: small number (legitimate gaps: NN-VC quality lift,
# vvenc kernel CPU-time distribution, eng-month estimates).
```

## Open questions (substantive — block promotion of dependent work)

The original 10 open questions are now mostly closed. Remaining gaps:

1. **NN-VC quality-lift magnitude** — actual bitrate-at-iso-VMAF lift
   on representative corpora. Requires running VVenC NN-VC vs. baseline
   on a representative corpus. No primary-source publishes a number for
   the fork's specific use case. **Stays open**, scoped to a future
   Tier-1 follow-up benchmark.
2. **HIP-portability of vvenc kernels** — actual CPU-time distribution
   per kernel on a real corpus. Requires `perf` profiling. **Stays
   open**, scoped to the Tier-2 prerequisite.
3. **Fraunhofer HHI VVenC private GPU-port roadmap** — public issue
   tracker is empty (verified `[verified]`); HHI may have non-public
   plans. Cannot be answered from public sources alone. **Stays open**;
   re-check on every quarterly Tier-3 revisit.

Closed (as of 2026-05-06) by primary-source verification:

1. AMD AMF VVC encode → **no**, verified.
2. Intel oneVPL VVC encode → API enums present (oneVPL 2.15+), but
   runtime ships **decode** on Lunar Lake only; encode not in any
   release notes through 2.16.0. Verified.
3. Intel Lunar Lake / Battlemage VVC decode → Lunar Lake **yes**,
   Battlemage **no**. Verified.
4. Khronos `VK_KHR_video_encode_h266` → **does not exist**, verified
   by registry 404 and zero Vulkan-Docs issues.
5. Mesa AV1 encode → Mesa 25.2 (RADV, August 2025). Verified.
6. Mesa VVC decode → not in Mesa Vulkan; Lunar Lake VVC decode reaches
   users via Intel iHD VA-API media driver + FFmpeg. Verified.
7. ZLUDA codec-workload coverage → zero (no NVENC/NVDEC module in
   tree). Verified.

## Related

- [ADR-0315](../adr/0315-vendor-neutral-vvc-encode-strategy.md) —
  the decision this digest feeds. Verified data points propagate into
  ADR-0315's `## Alternatives considered` matrix in the same PR.
- [ADR-0314](../adr/0314-vmaf-tune-vulkan-score-quick-win.md) —
  Tier-1 sibling, scoped separately, wires Vulkan scoring through
  `vmaf-tune`. This is the only concrete deliverable in the digest's
  Tier 1 today.
- [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md) — NVENC
  adapter ladder (h264 / hevc / av1 only). No h266 NVENC adapter
  exists or is planned (NVENC silicon does not ship H.266 encode).
- [ADR-0127](../adr/0127-vulkan-compute-backend.md) — Vulkan compute
  backend for scoring.
- [`tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py)
  — the existing CPU adapter, with NN-VC toggle.
- VVenC upstream: <https://github.com/fraunhoferhhi/vvenc>
- NVIDIA Video Codec SDK 13.0 NVENC Application Note (verified):
  <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-application-note/index.html>
- AMD AMF SDK encoder components (verified):
  <https://github.com/GPUOpen-LibrariesAndSDKs/AMF/tree/master/amf/public/include/components>
- Intel oneVPL `mfxstructures.h` (verified):
  <https://github.com/oneapi-src/oneVPL/blob/main/api/vpl/mfxstructures.h>
- Khronos `VK_KHR_video_encode_av1` (verified ratified Nov-2024):
  <https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_video_encode_av1.html>
- Phoronix on Mesa 25.2 RADV AV1 encode (verified Aug-2025):
  <https://www.phoronix.com/news/RADV-Merges-AV1-Encode>
- Phoronix on Intel VVC VA-API decode in FFmpeg (verified):
  <https://www.phoronix.com/news/VVC-VA-API-FFmpeg-Decode>
- Fraunhofer HHI VVenC issues (verified empty for GPU port):
  <https://github.com/fraunhoferhhi/vvenc/issues>
- ZLUDA repository (verified, v5 release 2025-10-02):
  <https://github.com/vosen/ZLUDA>
