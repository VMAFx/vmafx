- **The macOS tester bundle measures every open Metal state row in one run**
  (ADR-1496). The parity gate (`scripts/ci/cross_backend_parity_gate.py`) has a
  `metal` backend and a `--hold-exact <backend>` option that compares that
  backend's cells exactly at `--precision max` before an `exact_twins.d`
  fragment lists it; the bundle carries the gate and runs it on its four
  fixtures as `--backends cpu metal --hold-exact metal` (report section
  `metal_gate`, a check of the verdict). Every Metal parity test compares with
  `==` (ciede at the `1e-9` `LIBM_TWINS` bound) on the CUDA, HIP and SYCL
  twins' cases, adds the cases the open rows need (full-range 16-bit content,
  10- to 16-bit input with large differences, option sets, identical pairs, one
  frame, frames below 16 and 17 pixels, `adm_noise_weight=0`,
  `adm_enhn_gain_limit` 1.2 and 1.5, `motion3` and the motion SAD score), runs
  every case after a failure and prints a verdict per case, which the report
  keeps (`unit_tests.cases`). `tools/rc1-tester/image/metal-rows.json` maps each
  open Metal row to the cases, fixture metrics and gate cells that close it, and
  the report gives each row a verdict (`metal_rows`). Report schema 2 adds the
  three; schema 1 reports stay valid. The Metal tests also build on every host
  as self-tests with the CPU extractor in the twin's place (suite
  `metal-selftest`). Guides: `docs/usage/tester-image.md`,
  `docs/backends/metal/index.md`, `docs/development/cross-backend-gate.md`.
