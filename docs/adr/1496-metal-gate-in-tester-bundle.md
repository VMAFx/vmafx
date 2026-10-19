<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1496: The macOS tester bundle runs the parity gate's Metal cells and reports per state row what it measured

- **Status**: Accepted (Superseded-in-part 2026-10-06 by [ADR-1707](1707-rc3-exit-without-outside-hardware.md) for the condition that every Metal row is closed before rc.3)
- **Date**: 2026-10-03
- **Deciders**: Lusoris
- **Tags**: ci, metal, testing, parity, macos, fork-local

## Context

`v1.0.0-rc.3` is cut only when every open RC3 row of `docs/state.md` is
closed, the Metal rows included: the Metal twins are ported to the exact
designs of their CUDA, HIP and SYCL twins and then proven on a real Apple
device by a second report from an outside tester (Apple M4). No Apple device
runs anything for the project: the hosted macOS runner compiles the Metal
code and has no usable Metal device, so a Metal score exists only in a
tester's report from the macOS tester bundle (ADR-1493).

That report, as first published, could not close the rows. It compared every
Metal twin with the CPU on four 8- and 10-bit fixtures at default options, so
the rows whose defect shows only at 16 bits, with an option set, on a frame
below 17 pixels or on an identical pair were "not measured" by it, as each
row's lead said. Its unit tests were the old Metal parity tests, which
compared at places=4 and passed every known defect; the executable verdict
was all the report kept. And the parity gate had no `metal` backend
(`T-GATE-NO-METAL-BACKEND-2026-10-02`), so no gate ever compared a Metal twin.
A second tester run is expensive (an outside person's time); it has to
measure every Metal row at once.

## Decision

We give `scripts/ci/cross_backend_parity_gate.py` (and the single-feature
gate) a `metal` backend (`--metal_device`, the `integer_*_metal` names in
`BACKEND_EXTRACTOR_ALIASES`) and a `--hold-exact <backend>` option that
compares every cell of that backend exactly at `--precision max` (or at the
`LIBM_TWINS` bound for ciede), before any `exact_twins.d` fragment declares
it. The macOS tester bundle carries the gate (standard library only, its
fragments and the ADR files they cite) and runs it on every fixture as
`--backends cpu metal --hold-exact metal`, leaving out only features the
Metal twin cannot run on a fixture for a recorded reason (`float_ssim` at
1080p, an RC8 row). The gate run is a check of the report (`metal_gate`).
Every Metal parity test compares with `==` (ciede at the `LIBM_TWINS` bound),
runs every case after a failure and prints one `@case` line per case
(`core/test/metal_twin.h`); the report keeps those verdicts. A row map,
`tools/rc1-tester/image/metal-rows.json`, names per open Metal row the cases,
fixture metrics and gate cells that close it, and the report evaluates it
(`metal_rows`: pass, fail or not measured per row). Report schema 2 adds the
three. The same tests build as self-tests on every host, with the CPU
extractor standing in for the twin, so a wrong fixture or key fails in CI and
not on the tester's machine. Every Metal kernel is compiled with
`-std=metal3.1 -mmacosx-version-min=14.0`, the language revision and the
deployment target of the bundle's macOS 14 floor: without a target the offline
compiler stamps the kernels with the hosted runner's SDK version, and such a
metallib refuses to load on an older macOS, which would leave a tester's run
with nothing measured.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Declare the bundle's fixture comparison the Metal gate (an ADR, no gate change) | No gate change, no gate files in the bundle | Two definitions of "the gate" for one backend; the comparison runs default options only, so `float_ssim_lcs`, `float_ms_ssim_lcs`, `motion_debug` and the five-frame-window cells would never run on Metal; the row's own closing condition asks for the gate | The gate already holds the cells, the metric lists and the exactness rules; running it on the device measures more and needs no second contract |
| Run the gate at its default tolerances in the bundle | No new gate option | Metal cells would pass at `5e-5` with the defects the rows describe (per-block sums are 1e-7 to 1e-5 off), so a passing gate would prove nothing about exactness | The point of the run is the measurement a fragment cites |
| Declare the Metal twins exact (fragments) before the device run, so the gate compares at 0 without a new option | No new option | A fragment carries an `evidence:` line from a device measurement (ADR-1428); writing one before the measurement is the false record the fragment rule exists to prevent | `--hold-exact` measures without declaring; the fragment follows the report |
| Keep the old Metal tests and only add the missing cases | Smaller diff | The old tests compared at places=4 and stopped at the first failure: a passing test proved nothing and a failing one hid every later case | One run must give a verdict for every case |
| Leave the kernels' language revision and deployment target to the compiler | No build change | The offline compiler targets the build machine's SDK; a library stamped with a newer macOS than the tester's does not load ("deployment target ... not supported on this OS") | The run exists to measure the twins on the tester's Mac |
| Run each test case as its own process (a filter argument) | Per-case isolation | Changes the shared test runner for every test; one process per case multiplies device setup | The `@case` line gives per-case verdicts from one process |

## Consequences

- **Positive**: one tester run measures every open Metal row and says which it
  closes; the report is the evidence for closing a row and for a later
  `scripts/ci/exact_twins.d/<feature>.metal` fragment. The hosted macOS CI
  stays green before the ports land: every Metal case skips without a device.
- **Negative**: the bundle's run takes longer (the gate runs 22 features on
  four fixtures, each on the CPU and on Metal); the bundle carries about 40
  more files (the gate, 69 fragments, the cited ADRs). Tests that fail on
  today's twins are the point: the first run of a bundle built from a commit
  before the ports reports them failing.
- **Neutral / follow-ups**: a new Metal twin needs a gate feature, a parity
  test listed in `metal_parity_tests` and in `unit-tests-macos.txt`, and its
  rows in `metal-rows.json` (`core/test/test_metal_report_rows_contract.py`
  fails otherwise). After a passing device report, the maintainer closes the
  rows and adds the `.metal` fragments with the report as evidence.

## References

- [ADR-1493](1493-macos-tester-bundle.md) (the macOS tester bundle),
  [ADR-0214](0214-gpu-parity-ci-gate.md) (the parity gate),
  [ADR-1428](1428-exact-twins-fragments.md) (exact-twin fragments),
  [ADR-1426](1426-cuda-ciede-cpu-arithmetic.md) (`LIBM_TWINS`),
  [ADR-1460](1460-gate-speed-temporal-and-uncovered-twins.md) (every twin is a gate cell).
- State rows: `T-GATE-NO-METAL-BACKEND-2026-10-02` and the open Metal rows of
  `docs/state.md` (`tools/rc1-tester/image/metal-rows.json`).
- Source: `Q` (popup 2026-10-03): "Wait for Metal ports + report".
