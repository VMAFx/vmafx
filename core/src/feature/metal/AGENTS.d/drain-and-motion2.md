---
paths:
  - core/src/feature/metal/float_motion_metal.mm
  - core/src/feature/metal/integer_motion_metal.mm
  - core/src/feature/metal/integer_motion_v2_metal.mm
invariant: Every Metal extractor sets VMAF_FEATURE_EXTRACTOR_METAL; stream drains at end; frame-0 motion2 is emitted.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Rebase-sensitive invariants (end-of-stream drain + frame-0 motion2)

- **Every Metal extractor MUST set `VMAF_FEATURE_EXTRACTOR_METAL`**
  (`feature_extractor.h`, bit 7). `flush_context_serial`
  (`core/src/libvmaf.c`) drains backend's pending final-frame
  `collect()` *only* when extractor carries its backend flag —
  `#ifdef HAVE_METAL` branch keys off `VMAF_FEATURE_EXTRACTOR_METAL &&
  gpu_pending`, mirroring CUDA / HIP / SYCL drain blocks. Extractor
  that forgets flag silently drops its last submitted frame's score
  (generic submit/collect double-buffer leaves `collect(N)` pending).
  When adding new `<feature>_metal.mm`, OR flag into `.flags`
  alongside any feature-class flag (e.g.
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL`).
  All 17 registered Metal extractors set this flag
  (GAP-METAL-DISPATCH-FLAGS-ZERO-MODEL-FALLBACK resolved 2026-09-02;
  all 9 round-3/4 extractors promoted).
- **Frame-0 `motion2` emission contract.** Motion-family Metal
  extractors append `motion2 = 0.0` at index 0, no-op at index 1, and
  `min(prev, cur)` at index − 1 for index ≥ 2 — byte-identical to
  `integer_motion_metal` and HIP / CUDA twins. `float_motion_metal`
  was previously missing index-0 append and double-wrote at index 1
  (fixed fix/metal-drain-motion2, 2026-06-20). Never "simplify"
  index-0 / index-1 split away.
