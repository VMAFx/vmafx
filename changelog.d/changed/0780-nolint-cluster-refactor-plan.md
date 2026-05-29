## NOLINT cluster audit and refactor plan (ADR-0780)

Swept all 218 NOLINT annotations in `core/src/` for clusters of five or more identical
suppressions per file. Identified five clusters (71 annotations total):

- **SYCL stride arithmetic** (`bugprone-implicit-widening`, 12 annotations): scheduled
  for removal via explicit `(ptrdiff_t)` casts — no semantic change.
- **GPU slab allocator** (`performance-no-int-to-ptr`, 21 annotations): missing ADR
  citations (ADR-0278 non-compliant); scheduled for consolidation behind a shared
  `SLAB_FIELD` macro in `core/src/feature/gpu_slab.h`.
- **SYCL `misc-const-correctness`** (14 annotations): fold into existing per-file
  `NOLINTBEGIN`/`NOLINTEND` block; no new annotation site needed.
- **CPU ADM band-processing** (`readability-function-size`, 13 bare annotations):
  scheduled for `NOLINTBEGIN` block consolidation with ADR-0141 citation.
- **SYCL kernel entry-points** (`readability-function-size`, 11 annotations):
  load-bearing per ADR-0141; no change.

Research digest: `docs/research/nolint-cluster-audit-2026-05-29.md`.
Follow-up PRs A–C are independent and can be executed in parallel worktrees.
