<!-- markdownlint-disable MD013 -->
# ADR-0315: Vendor-neutral VVC encode strategy — tiered Tier-1-now / Tier-2-backlog / Tier-3-revisit

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris, lawrence
- **Tags**: codecs, vvc, h266, gpu, hip, sycl, vulkan-video, vmaf-tune, nn-vc, fork-local

## Context

The fork ships VVC (H.266) encode today through a CPU-only adapter
([`tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py`](../../tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py))
that wraps Fraunhofer HHI's reference encoder.

**Verified at write time** (2026-05-06 follow-up; supersedes the
2026-05-05 partial verification): NVENC SDK 13.0 supports only
H.264 / HEVC / AV1 (no H.266 encode); AMD AMF SDK v1.5.0
(2025-10-29) ships only `VideoEncoderVCE.h` (H.264) /
`VideoEncoderHEVC.h` / `VideoEncoderAV1.h` — no
`VideoEncoderH266.h` / `VideoEncoderVVC.h`; Intel oneVPL 2.16.0
(2025-12-08) public API defines `MFX_CODEC_VVC` plus full VVC
profile/level enums (added in 2.15.0, 2025-04-11), but the runtime
(`intel/vpl-gpu-rt`) wires VVC through to **decode** only, on Lunar
Lake silicon. **No vendor ships hardware VVC encode** in any
shipping SDK at write time. Intel Lunar Lake (Core Ultra 200V,
Xe2 iGPU) is the first/only consumer silicon with hardware VVC
**decode**; Battlemage (Arc B-series) does not ship VVC decode.
`VK_KHR_video_encode_h266` does not exist in the Khronos registry
(verified 404). See Research-0085's "Verification status" table
for primary-source links.

The user (lawrence, 2026-05-05) asked whether a GPU-accelerated
VVC encode path that does not require NVIDIA hardware is feasible.
The honest answer today: **no vendor ships hardware VVC encode**, so
the question reduces to "which non-NVIDIA-tied software encoder +
GPU-accelerated tooling around it can the fork integrate".

Research-0085 (now Active, 2026-05-06) surveyed the landscape and
identified four candidate paths: (a) wait for some vendor to ship
hardware VVC encode silicon (passive); (b) HIP-port VVenC's hot
kernels (open-source software acceleration; eng-months
`[UNVERIFIED]` until profiled, vendor-coverage AMD); (c) wait for
Vulkan Video `VK_KHR_video_encode_h266` ratification + driver
landing — verified that the extension does **not exist today**
(Khronos registry 404), and the AV1 precedent (Nov-2024 ratification
→ Aug-2025 RADV implementation) puts realistic delivery multi-quarter
beyond any future ratification; or (d) document NN-VC + the existing
Vulkan **scoring** backend as the de-facto vendor-neutral story
today.

Forces:

- **Power of 10 / scope discipline** — the fork has limited engineering
  bandwidth; a 6-month HIP-port commitment crowds out tiny-AI and
  Vulkan-scoring backlog items.
- **Demand-pull principle** — fork-local efforts only ship when a
  user reports the gap as binding (precedent:
  [ADR-0009](0009-batch-a-upstream-port-strategy.md)).
- **Hardware availability** — the fork's CI matrix does not include
  RDNA 3/4 or Intel PVC nodes; a HIP port without a CI gate cannot
  satisfy [ADR-0214](0214-cross-backend-numerical-parity.md)'s
  GPU-parity rule.
- **Vendor-neutrality value** — non-NVIDIA users today have *zero*
  GPU acceleration on the VVC encode side; the gap is real, not
  theoretical.

## Decision

We will adopt a three-tier strategy. **Tier 1 ships today**:
document NN-VC as the fork's vendor-neutral H.266 GPU story (it
already runs on any ONNXRuntime EP) and wire the existing Vulkan
backend to `vmaf-tune` for GPU-accelerated scoring of CPU-encoded
VVC bitstreams (sibling [ADR-0314](0314-vmaf-tune-vulkan-score-quick-win.md),
scoped separately). **Tier 2 stays in the backlog**: a HIP port of
VVenC's motion-estimation, transform, and loop-filter kernels, gated
on three triggers (user-reported throughput pain on a real corpus,
Tier-1 NN-VC docs adopted in production, RDNA 3/4 or PVC CI machine
available). **Tier 3 is revisited quarterly**: a
`VK_KHR_video_encode_h266`-based libvmaf-side encode adapter, gated on
Khronos ratification and at least one shipping driver implementation.

Research-0085 is the authoritative source survey; this ADR is the
decision artifact that points back to it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **A. Ship CUDA-VVC NVENC adapter** (mirror [ADR-0290](0290-vmaf-tune-nvenc-adapters.md) for h266) | Would help RTX-40 users **if NVENC supported H.266**; reuses adapter ladder pattern | **Verified false premise**: NVENC SDK 13.0 supports only H.264 / HEVC / AV1 (no H.266 encode). There is no `h266_nvenc` to wire up. | **Rejected** as factually impossible at write time. Re-evaluate if NVIDIA ships VVC encode silicon in a future SDK. |
| **B. Wait for `VK_KHR_video_encode_h266` ratification** | Zero effort; eventually delivers vendor-neutral hardware encode on every Vulkan 1.4+ driver; reuses the fork's existing Vulkan loader / queue / DMABUF plumbing | **Verified non-existent** at write time (Khronos registry 404; zero Vulkan-Docs issues); AV1 precedent (`VK_KHR_video_encode_av1` ratified Nov-2024, Vulkan 1.3.302 → Mesa RADV implementation Aug-2025) shows ~9-month spec-to-driver lag *after* eventual ratification; abandons users who need a non-NVIDIA path **today** | Rejected as a standalone strategy. Reframed as Tier 3 (revisit quarterly) inside the chosen tiered approach. |
| **C. HIP port of VVenC hot kernels (immediate)** | Vendor-neutral on AMD silicon; precedent for fork-side GPU ports exists ([ADR-0033](0033-hip-applicability.md)) | Eng-months `[UNVERIFIED]` until profile run; rebase burden against vvenc's release cadence; no CI hardware available; no demand signal yet beyond the question itself; verified that VVenC upstream has **no public CUDA/HIP/SYCL/OpenCL/Vulkan port** in its issue tracker | Rejected as immediate Tier 1; queued as Tier 2 with explicit demand-pull triggers. |
| **D. Tiered approach (chosen)** | Ships *something* vendor-neutral today (NN-VC docs + Vulkan scoring); preserves option value on the HIP port without burning engineering hours speculatively; revisits Vulkan Video on a calendar | Communicates "no GPU vendor ships VVC encode silicon in 2026; we mitigate via NN-VC + Vulkan scoring"; relies on CPU encode for near-term throughput | **Chosen.** Matches the fork's demand-pull pattern; gives users an honest answer + a non-zero GPU contribution (NN-VC + scoring) immediately; preserves room for Tier 2 / Tier 3 to arrive when triggers fire. |
| **E. ZLUDA-hosted hypothetical CUDA-VVC** | In theory runs CUDA codecs on AMD/Intel | No open-source CUDA VVC encoder exists; verified that ZLUDA's module tree covers cuBLAS / cuFFT / cuDNN / cuSPARSE / Driver+Runtime APIs only — **no NVENC/NVDEC module**, so even if a CUDA VVC encoder appeared it could not run on ZLUDA without further work; reviewers would reasonably reject the production posture | Rejected as not actionable. |

## Consequences

- **Positive**:
  - User question answered honestly today: NN-VC + Vulkan scoring
    deliver the vendor-neutral GPU contribution that exists in 2026.
  - Engineering bandwidth preserved for tiny-AI / Vulkan-scoring
    backlog rather than committed to a 6-month HIP port without a
    demand signal.
  - Tier 2 / Tier 3 stay tracked in the backlog with explicit
    triggers, preventing them from being silently forgotten.
- **Negative**:
  - **No** users — NVIDIA, AMD, or Intel — get *hardware* VVC encode
    acceleration in the near term; no GPU vendor ships VVC encode
    silicon in 2026 per write-time verification (NVIDIA SDK 13.0,
    AMD AMF v1.5.0, Intel oneVPL 2.16.0 all checked directly). The
    only real levers are NN-VC quality (any-GPU) and CPU SIMD
    throughput.
  - The fork's "vendor-neutral codec story" is partial — strong on
    AV1 and HEVC (NVENC + AMF + QSV adapters all exist), weaker on
    H.266 (no GPU vendor ships VVC encode hardware).
- **Neutral / follow-ups**:
  - **No** `h266_nvenc` adapter follow-up is planned: NVENC silicon
    does not ship VVC encode at write time. If NVIDIA adds it in a
    future SDK, an analogue of [ADR-0290](0290-vmaf-tune-nvenc-adapters.md)
    would land then.
  - Tier 2 trigger conditions live in Research-0085 §"Tier-1 → Tier-2
    transition gate"; reviewers monitor user reports for the binding
    signal.
  - Tier 3 quarterly revisit owner is whoever runs the next
    `/sync-upstream` cycle after each Khronos Working Group public
    cadence event.
  - The fork's `docs/codecs/vvc.md` (forthcoming) will document the
    user-facing matrix: which codec on which hardware on which day.

## References

- Research-0085: [Vendor-neutral VVC GPU encode landscape](../research/0085-vendor-neutral-vvc-encode-landscape.md)
  — source survey, citations, cost/risk/value matrix.
- [ADR-0290](0290-vmaf-tune-nvenc-adapters.md) — NVENC adapter ladder
  pattern that an h266_nvenc adapter would mirror.
- [ADR-0314](0314-vmaf-tune-vulkan-score-quick-win.md) — Tier-1
  sibling, scoped separately, wires Vulkan scoring through
  `vmaf-tune`.
- [ADR-0033](0033-hip-applicability.md) — prior HIP applicability
  survey for libvmaf features.
- [ADR-0127](0127-vulkan-compute-backend.md) — Vulkan compute backend
  (scoring).
- [ADR-0009](0009-batch-a-upstream-port-strategy.md) — fork's
  demand-pull pattern for fork-local engineering effort.
- [ADR-0214](0214-cross-backend-numerical-parity.md) — GPU-parity
  rule that Tier 2 has to satisfy on landing.
- Source: `req` — paraphrased user request: "investigate whether the
  fork can offer GPU-accelerated VVC encode without NVIDIA hardware,
  and recommend a tiered path that ships something today" (lawrence,
  2026-05-05).

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

The deliverable for a strategy-shape decision is the documented
tier policy. Verification consists of confirming the tiers are
operative in tree:

- **Tier-1 (now)**: NN-VC + Vulkan **scoring** path is live.
  `tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py` is the
  CPU baseline VVenC adapter; ADR-0314 (this sweep, Accepted)
  ships `--score-backend=vulkan`.
- **Tier-2 (backlog)**: HIP VVenC port — not started, awaiting
  demand-pull trigger per ADR-0009 precedent.
- **Tier-3 (revisit)**: Vulkan Video VVC encode —
  `VK_KHR_video_encode_h266` does not exist in the Khronos
  registry; revisit when ratification + driver landing are
  observed (companion Research-0085 tracks the verification table).
- Verification command:
  `ls tools/vmaf-tune/src/vmaftune/codec_adapters/vvenc.py`.
