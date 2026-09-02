<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
### HIP backend audit (Research-0755)

Completed a full audit of `core/src/hip/` and `core/src/feature/hip/`
covering `extern "C"` name-mangling correctness, pinned-host buffer
leak patterns, `AdmBufferHip` struct-by-value in kernel signatures, and
scaffold-vs-real-kernel classification.

Results: no P0 blockers found. P1: `AdmBufferHip` passed by value in
7+ kernel signatures in `integer_adm/adm_csf.hip` and `adm_cm.hip`
(~272 bytes; recommend pointer-passing). P2: `dispatch_strategy.c`
remains a stub; cross-backend ULP gate runs not yet present for the
newer extractors; CAMBI HIP terminus (ADR-0345 Phase 3) confirmed landed.

See `docs/research/0755-hip-backend-audit-20260529.md`.
