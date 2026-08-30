# ADR-1001: SYCL parity round 5 — CAMBI CPU vs. SYCL parity gate

- **Status**: Accepted
- **Date**: 2026-06-04
- **Deciders**: Lusoris
- **Tags**: `sycl`, `test`, `gpu`, `parity`, `kernel-coverage`, `cambi`, `fork-local`

## Context

ADR-0957 (round 4) claimed "16 of 18 registered SYCL extractors have an
active CPU vs. SYCL parity test", treating the two dormant SpEED twins as
the only gap. On inspection, `integer_cambi_sycl.cpp` was counted as covered
by `test_integer_cambi_sycl.c`, but that file explicitly states: "This is NOT
a numerical-correctness test." It verifies only registration + finite/non-negative
output, not that the SYCL kernel produces scores within ADR-0214 tolerance of
the CPU scalar path. The true active coverage was therefore 15 of 18, with
`cambi_sycl` being the silent gap.

CAMBI feeds the `vmaf_v0.6.1neg` and `vmaf_4k_v0.6.1` model lineages. A
silent SYCL drift in the multi-scale banding detector would bias every CHUG
re-extract that uses those models on Intel Arc without triggering any CI gate.

## Decision

Add `core/test/test_sycl_cambi_parity.c` — a CPU vs. SYCL parity test for
`cambi_sycl` using a 256x256 quantised-gradient fixture (above the
216x216 `CAMBI_MIN_WIDTH_HEIGHT` minimum) at places=4 (1e-4) tolerance per
ADR-0214. Register it in `core/test/meson.build` inside the existing
`if get_option('enable_sycl')` block, suite `['fast', 'gpu']`. The test
skips with a `[skip: no SYCL device]` message when no oneAPI runtime is
present, matching the pattern from all prior rounds.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Elevate `test_integer_cambi_sycl.c` to a parity test | Single file, no extra meson entry | The smoke test has a different scope documented in its header; changing it in place would confuse future readers about when the parity assertion was added and by which PR | New file keeps the audit trail cleaner |
| Raise tolerance to 5e-3 (ssimulacra2 precedent) | Simpler uniform rule | CAMBI is integer arithmetic; empirically bit-tight. 1e-4 is the correct ADR-0214 default | — |

## Consequences

- **Positive**: `cambi_sycl` now has an active CI-gated parity gate alongside
  all other registered SYCL extractors. ADR-0957 counter corrects to 16/18
  active + 2 dormant SpEED, i.e. `cambi_sycl` is no longer an uncovered gap.
- **Negative**: minor CI cost increase — one additional GPU test per SYCL-enabled
  run. Fixture is 256x256 and single-frame; wall-clock impact is negligible.
- **Neutral**: `test_integer_cambi_sycl.c` retains its smoke-test role unchanged;
  this ADR adds a complementary numerical gate, not a replacement.

## References

- ADR-0214 — GPU parity tolerance table (`FEATURE_TOLERANCE`).
- ADR-0957 — SYCL kernel coverage round 4 (where the CAMBI gap was first
  mis-counted as covered).
- ADR-0884 — SYCL kernel coverage round 2 (template for this series).
- `req`: the user requested parity coverage for all missing SYCL kernels in the
  round-5 task brief; CAMBI was listed among the suspected gaps.
