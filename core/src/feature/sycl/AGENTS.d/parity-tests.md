---
paths:
  - scripts/ci/cross_backend_parity_gate.py
  - core/test/test_sycl_*_parity.c
invariant: Every shipping SYCL kernel here must have scalar-vs-SYCL parity test.
---
<!-- markdownlint-disable MD013 MD060 -->
# Per-kernel parity-test invariant (rounds 1–3)

Every SYCL feature kernel here has scalar reference and
`core/test/test_sycl_<kernel>_parity.c` gate. Most use ADR-0214 places=4
(1e-4) tolerance; `motion_add_uv` uses ADR-1326's exact fixed-point oracle
because its CPU float semantic twin has different arithmetic. Coverage matrix
below tracks which SYCL kernel maps to which CPU twin and which parity test.
**On rebase**: if SYCL kernel renamed or new one added, parity test name +
ADR-0884 / ADR-0946 backlog must update in same PR.

- [ADR-0214](../../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate.
- [ADR-0985](../../../../../docs/adr/0985-sycl-parity-divergence-2026-06-03.md) —
  SYCL SSIMULACRA 2 parity divergence and recurrence resolution.

## Per-kernel parity-test invariant (ADR-0214 + ADR-0868 + ADR-0884)

**Every shipping SYCL kernel here must have scalar-vs-SYCL parity test
under [`core/test/`](../../../../test/), wired into
[`core/test/meson.build`](../../../../test/meson.build) with suite
`['fast', 'gpu']`.** parity test normally asserts headline score
matches its CPU scalar reference within ADR-0214 places=4 (`1e-4`);
`motion_add_uv` instead matches arithmetic-identical fixed-point oracle
within ADR-1326's derived host-double bound. Tests skip cleanly when no SYCL
device visible — mirrors `[skip: no SYCL device]` pattern in
[`test_sycl_motion3_parity.c`](../../../../test/test_sycl_motion3_parity.c).

Coverage matrix:

| Kernel TU | Parity test | ADR |
|---|---|---|
| `float_*_sycl.cpp`, `speed_*_sycl.cpp`, `ssimulacra2_sycl.cpp`, `integer_moment_sycl.cpp`, `integer_psnr_hvs_sycl.cpp` | (round 3 backlog — see ADR-0884) | — |

**Rebase-sensitive**: adding new SYCL kernel TU, same PR
must add matching `test_sycl_<kernel>_parity.c` and meson
wiring. `/cross-backend-diff` skill = dev-time tool only,
does NOT run in CI on every PR; only in-tree repository-runner parity
tests catch per-kernel regressions automatically.
