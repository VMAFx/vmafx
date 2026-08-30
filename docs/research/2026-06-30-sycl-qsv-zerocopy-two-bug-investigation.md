<!-- markdownlint-disable MD013 MD060 -->
# SYCL QSV Zero-Copy — Two-Bug Investigation (Phase 3 fix digest)

**Branch:** `fix/libvmaf-sycl-zerocopy-nan`
**Date:** 2026-06-30
**Phase:** 3 (verify → reopen) — supersedes the Phase-1 diagnosis digest
([sycl-zero-copy-nan-diagnosis.md](sycl-zero-copy-nan-diagnosis.md)) and the FIX-01 attempt.
**ADR:** [ADR-1121](../adr/1121-sycl-qsv-zerocopy-p010-normalization.md)
**Hardware:** Intel Arc A380 (DG2), `/dev/dri/renderD128`, oneAPI/Level Zero.

---

## Summary

`libvmaf_sycl` (FFmpeg zero-copy filter on QSV-decoded VA-API surfaces) returned
`VMAF score: nan` with `integer_motion` in the thousands and a *varying* NaN count
across runs, while the host-upload `libvmaf` filter on the same machine and pair was
exact. Two prior fix attempts mis-scoped the defect to the `vmaf_read_pictures_sycl`
orchestration loop (single-slot theory, then the "FIX-01" slot toggle). FIX-01 made
the slot alternate `0,1,0,1…` but did not move the numbers — verification (VERF-01/02/03)
falsified it.

A fresh scientific-method pass eliminated **16 hypotheses** and found **two
independent root causes**, neither in the orchestration loop:

1. **Shared QSV/VA surface pool** → cross-decoder contamination (the
   `sycl_dis[N] == sycl_ref[N±1]` signature).
2. **P010 MSB/LSB pixel-range mismatch** → 64× `integer_motion` inflation and NaN.

Fixing both (FIX-03 invocation + FIX-04 normalization) brings the zero-copy path to
parity with the CPU oracle: 0 NaN, VMAF Δ 0.0014, motion Δ 1e-6 over 500 frames.

---

## Bug 1 — shared QSV/VA surface pool

**Signature.** `VMAF_SYCL_CHECKSUM` showed the distorted-buffer CRC equal to a
reference-buffer CRC one display-order frame away: `sycl_dis[10] == sycl_ref[9]`
(pre-FIX-02), shifting to `sycl_dis[10] == sycl_ref[11]` after the coherency-sync
experiment. Earlier sessions read this as a libvmaf buffer-aliasing/slot bug.

**Root cause.** When both decoders are created against the same `-hwaccel_device`,
FFmpeg builds a shared `AVHWFramesContext` → one `mfxSession` → one
`vaCreateSurfaces` pool. The AV1 (dis) and HEVC (ref) decoders hold *distinct*
`mfxFrameSurface1` wrapper objects, but those wrappers map to the **same physical
`VASurfaceID`s**. `Data.Locked` on the AV1 wrapper does not prevent the HEVC decoder
from writing `ref[N±1]` (decode-order, B-frame look-ahead) into that same VA surface.
The filter then imports that surface for `dis[N]` and gets reference content.
`imported_ptr_ref` was observed constant across frames (Level Zero reuses the VA
surface's backing), consistent with a single shared pool.

**Fix (FIX-03).** Give each decoder its own session:
`-init_hw_device qsv=qsv_ref@va0 … -init_hw_device qsv=qsv_dis@va0`. Surface IDs no
longer overlap; contamination is impossible. This is a usage contract — libvmaf
cannot observe FFmpeg's session topology from inside the filter, so it is documented
rather than enforced in code (see ADR-1121 Alternatives).

**Why not `DMA_BUF_IOCTL_SYNC` (FIX-02).** A cross-engine (VCS decoder → compute)
cache-coherency flush was hypothesized to fix a stale-read. It was implemented and
tested: contamination **persisted**, the stale-frame offset merely shifted
(`ref[9]`→`ref[11]`). Eliminated as the fix. It was briefly kept as defensive
hardening, then **removed**: its `SYNC_START` is a blocking decoder-fence wait
that serialises decode→compute and cut 4K throughput (39→33 fps at 22k frames)
with zero correctness benefit — `vaSyncSurface()` already establishes decode
completion and the in-order SYCL queue orders the de-tile after the import.

## Bug 2 — P010 MSB/LSB pixel-range mismatch

**Signature.** `integer_motion` (temporal self-difference of the reference)
SYCL/CPU ratio = exactly **64.0× = 2^(16−10)**; downstream `adm2 = -nan` (negative
under a root / 0÷0). VMAF mean ~34 or `nan` instead of ~97.

**Root cause.** VA-API stores 10-bit (P010) / 12-bit (P012) samples MSB-aligned:
`V_MSB = V_LSB << (16−bpc)`. The VMAF feature kernels expect LSB-aligned
`bpc`-bit integers (`[0, 2^bpc−1]`). The CPU `libvmaf` path never hits this because
FFmpeg auto-inserts a `P010LE → YUV420P10LE` conversion (a `>>6` shift) to satisfy
the filter's `FILTER_PIXFMTS`, which lists `YUV420P10LE` but **not** `P010LE`. The
SYCL import paths (readback memcpy, DMA-BUF LINEAR / Tile4 / Y-tiled) copied the raw
MSB-aligned bytes, so every pixel was 64× too large.

**Fix (FIX-04).** Right-shift every luma `uint16_t` by `(16−bpc)`. On the tiled
hot path (Tile4 / Y-tiled) the shift is **fused into the de-tile store** — each
sample is shifted as it is written, no extra kernel and no second memory pass.
The rare LINEAR D2D / readback paths have no per-sample store to fuse into, so
they keep a standalone `launch_p010_normalize()` kernel chained into
`vmaf_sycl_set_detile_event()`. No-op for 8-bit NV12. Luma-only: the import
transfers only the Y plane and VMAF is a luma metric, so chroma needs no
normalization. (An earlier revision used the standalone kernel on *all* paths;
that second full-plane pass — outside the replayed combined graph — cost ~15%
throughput at 4K and was replaced by the fusion.)

---

## Verification (de-contaminated oracle)

The pre-existing D-03 baseline (VMAF 96.133894, motion max 26.627813) was itself
measured under Bug 1 and is wrong. With separate sessions the CPU oracle is **VMAF
97.2350, motion max 26.6935**.

| Metric | SYCL (FIX-03+04) | CPU oracle (FIX-03) | Gate |
|---|---|---|---|
| NaN count | 0 | 0 | PASS |
| VMAF mean (500f) | 97.2336 | 97.2350 (Δ 0.0014) | PASS |
| integer_motion max | 26.6934 | 26.6935 (Δ 1e-6) | PASS |
| 30-frame same-column parity | vmaf 97.51117 / im2 max 1.15262 | vmaf 97.50843 / im2 max 1.15262 | PASS |

Reproducer: `.planning/phases/03-verify/verify-fix04.sh` (Shadow's Edge 4K HEVC→AV1,
Intel Arc A380). Netflix CPU golden assertions are untouched — this baseline lives
only in the fork's verification artifacts.

## Hypotheses eliminated (selected)

- De-tile kernel (Tile4 / Y-tiled swizzle) — `VMAF_SYCL_FORCE_READBACK=1` reproduced
  identical NaN; ruled out in Phase 1.
- SYCL feature kernels — correct via the host-upload path (15 GPU parity tests pass).
- Single-slot / slot-toggle orchestration (FIX-01) — slot alternation went live but
  numbers unchanged.
- `DMA_BUF_IOCTL_SYNC` as the contamination fix — persisted, only offset shifted.
- Hardware async race as the *original* cause — pre-fix output was byte-identical
  across runs (deterministically wrong, not a race). The post-FIX-01 non-determinism
  was a separate artifact of that abandoned change.

## Performance follow-up — combined graph is a net loss on the zero-copy path

After correctness was fixed, a 22k-frame 4K run showed throughput drop from ~39 to
~33 fps. Bisecting the cause: the per-frame de-tile import work is submitted live
(outside the replayed combined graph), and the graph's compute-barrier on
`last_detile_event` serialises decode→compute. Empirically, disabling the graph
(`VMAF_SYCL_NO_GRAPH=1`, i.e. direct dispatch) raised throughput to ~42 fps —
**above** the pre-fix baseline — and produced **byte-identical** output (sha256
match vs graph mode), deterministic across runs. So the SYCL combined graph
(ADR-0483), whose area-threshold default selects it at ≥720p, is the wrong choice
for the VA-import path: it costs ~15–25% at 4K for zero numeric benefit. The fix
defaults the zero-copy path (`state->has_imported`) to direct dispatch via a
`va_import_path` flag on `vmaf_sycl_select_strategy()`, checked after the env
overrides so `VMAF_SYCL_USE_GRAPH=1` still forces graph. The earlier
fuse-into-de-tile + `DMA_BUF_IOCTL_SYNC`-removal changes reduce per-frame work but
were secondary; the graph default was the dominant lever.
