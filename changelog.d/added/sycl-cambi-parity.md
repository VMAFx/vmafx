### SYCL: CAMBI CPU vs. SYCL parity gate (ADR-1001, round 5)

`test_sycl_cambi_parity.c` is a new CI-gated numerical parity test for the
`cambi_sycl` extractor. It asserts that the SYCL multi-scale banding kernel
matches the CPU scalar path within places=4 (1e-4) on a 256×256
quantised-gradient fixture designed to produce a non-trivial CAMBI score.

The pre-existing `test_integer_cambi_sycl.c` was explicitly a smoke test only
(registration + finite output). ADR-0957 incorrectly counted it as a parity
gate; this test fills the actual gap, raising the active SYCL parity coverage
to 17/18 registered extractors (the two remaining dormant SpEED twins activate
automatically once their build wiring lands per ADR-0957).

The test skips gracefully when no SYCL device is present, matching the pattern
from all prior parity rounds.

References: ADR-1001, ADR-0957, ADR-0214.
