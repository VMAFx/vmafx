<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
## [perf] CUDA F3 struct-by-value kernel audit (ADR-0756)

Fork-wide audit of every `__global__` kernel accepting a `VmafCudaBuffer`,
`VmafPicture`, or `AdmBufferCuda` argument by value — the "F3" pattern
identified in PR #93. Identifies 20 kernel variants across 8 metric families
where the struct copy hides pointer aliasing information from ptxas and
prevents `ld.global.nc` emission. Severity-ranked by measured DRAM throughput
from PR #77 ncu profiles. Top-5 dispatch PRs defined; `ms_ssim_vert_lcs` is
priority-1 (structurally identical to the already-fixed `ssim_vert_combine`,
expected -4 to -6% duration at 1080p).
