<!-- markdownlint-disable MD013 MD024 MD060 -->
# ADR-1342: RC1 external tester evidence bundle

- **Status**: Accepted
- **Date**: 2026-09-26
- **Deciders**: Kilian, VMAFx Core Team
- **Tags**: rc1, testing, diagnostics, backends, report, packaging

## Context

RC1 needs external correctness and environment evidence from hardware the
maintainer does not own. Existing primitives are fragmented: `vmaf` can select
a backend, vendor utilities expose some devices, Meson runs correctness tests,
and several later-phase benchmark/training tools exist. None creates one
portable report that records whether an explicit backend state initialized and
produced a bounded correctness result against CPU evidence.

That check must fail closed. A zero process status alone is insufficient: a
missing output file, unavailable fixture, implausible score, or unexpected root
backend state must not be published as accelerator success. Reports also need
enough provenance to replay the attempt without collecting device serial
numbers or exposing a tester's home-directory path by default.

The release sequence puts correctness/build/report readiness in RC1,
benchmarking and tuning in RC2, and real training in RC3. The RC1 collector
therefore inventories later-phase tools but does not execute them.

## Decision

Add the runtime-dependency-free launcher
`tools/rc1-tester/vmaf-rc1-report` and the external tester guide at
`docs/usage/rc1-tester-guide.md`.

1. `probe` records bounded OS, CPU, toolchain, accelerator-visibility, VMAFx
   version, and binary-hash evidence. Its packaged argv-only process runner
   enforces process-group timeouts and a shared output ceiling without a
   repository-only runtime import.
2. `validate` and `bundle` require one or more explicit `--backend` selectors.
   There is no `auto` validation mode. The validator always runs CPU first;
   when the tester requested only accelerators, that CPU row is labelled as an
   automatic correctness reference.
3. Each attempt consumes frames 0 through 3 from the checked-in 576x324
   `testdata` pair and passes `model/vmaf_v0.6.1.json` explicitly. Frame 3 has
   non-zero `integer_motion2`, so the bounded run reaches temporal model
   behavior. Missing binary, model, fixture, pinned CPU snapshot, or output is
   incomplete/failing evidence, never a version-only PASS.
4. CPU PASS requires process status zero, `backend_used=cpu`, the complete
   four-frame model-metric schema, VMAF scores inside `[0, 100]`, internally
   consistent pooled VMAF metrics, and per-frame agreement with the first four
   rows of `testdata/scores_cpu_576.json` within `5e-5` absolute tolerance.
   Accelerator PASS applies the same checks and compares its model-metric
   frames with the CPU attempt from that invocation.
5. ADR-0214 already owns `5e-5` for the feature metrics listed in its parity
   gate. This reporter adopts the same conservative bound for those model
   inputs and for overall per-frame VMAF; ADR-0214 did not previously define an
   overall-model VMAF gate.
6. `bundle` writes one archive containing human- and machine-readable reports,
   exact argv/environment overrides, requested/observed backends and device
   ordinal, separately labelled collector
   checkout revision, binary/model/fixture SHA-256 values, raw per-backend
   logs/strict parsed JSON, plus integrity checksums. It still writes evidence
   on failure or backend-unavailable status; non-finite failed-run values are
   encoded as JSON `null`.
7. Shared text redacts repository/home prefixes. Probes avoid serial-number and
   GPU-UUID queries, accelerator driver fields are honest about unavailable
   data, archive metadata is normalized, and documentation tells testers to
   inspect the result before sharing.
8. Exit `0` means every requested backend passed; `1` means failed evidence; `2`
   means incomplete evidence; `100` preserves the explicit-backend unavailable
   contract from ADR-0498 and ADR-0543.
9. Unix users execute the launcher directly. Windows users invoke it with
   Python and may resolve `vmaf.exe` from explicit/default build paths.
10. `--device-index` selects the runtime-visible accelerator ordinal. Native
   device flags carry SYCL/HIP/Metal indices; CUDA uses a recorded
   `CUDA_VISIBLE_DEVICES=N` override because its VMAF API has no device-index
   field. HIP discovery preserves distinct `rocm-smi` device rows.
11. PASS proves bounded emitted-metric correctness and root backend-state
   initialization for this four-frame fixture. `backend_used` does not prove
   which individual feature implementation ran, so this remains distinct from
   per-feature dispatch evidence and the compiled backend suites.
12. The module has package metadata and runs through the repository's
    `rc1_tester` nox session, Makefile Python lint targets, and hosted
    `RC1 Tester Report` job. The job is both required and strict-must-report.
    Runtime has no third-party dependency; the `dev` extra is test tooling only.
13. `SHA256SUMS` hashes payloads other than itself and the manifest; the manifest
   repeats those hashes and hashes `SHA256SUMS`; the CLI prints a separate
   whole-archive hash. No file self-hashes and no authenticity claim is made.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| :--- | :--- | :--- | :--- |
| **Dedicated zero-dependency in-tree launcher (chosen)** | Cross-platform Python, isolated from tuning/training dependencies, testable process boundary, source checkout retains fixtures. | Adds a small maintained module and report schema. | It is the smallest surface that can validate, redact, and package evidence honestly. |
| Extend `vmaf-tune` | Reuses an existing CLI/package. | Couples RC1 correctness collection to RC2 tuning dependencies and concepts. | Violates the release-phase boundary and increases tester setup. |
| Shell-only collector | Very small initial file count. | Weak Windows portability and fragile structured-output/privacy handling. | Does not meet the cross-platform report requirement. |
| Treat exit zero or `--help` selectors as backend evidence | Minimal implementation. | Publishes false PASS when JSON is absent, the root execution state differs, or emitted scores are wrong. | Contradicts the explicit-backend correctness contract. |

## Consequences

- **Positive**: CPU, CUDA, SYCL, HIP, and Metal testers can return one bounded,
  replayable report without running a benchmark or training workload.
- **Negative**: The report schema and CLI become a supported tester surface and
  need compatibility review when VMAFx output changes.
- **Neutral / follow-ups**: Binary hash and checkout revision identify separate
  artifacts but do not prove build provenance; reviewed Meson logs accompany
  build failures. RC2 must make benchmark tooling cross-platform and
  close its documented HIP/Metal coverage gaps before soliciting performance
  data. RC3 owns real training.

## References

- `req`: "check that people that thest rc1 actually can use all tools they need to bench and test on their hardware and create reports that they can send me so that I can fix and tune further?... something like that?"
- `req`: "move benching to rc2"
- `req`: "move the real training to rc3"
- [Research-1342](../research/1342-rc1-external-tester-report-bundle.md)
- [RC1 external tester guide](../usage/rc1-tester-guide.md)
- [ADR-0498](0498-vmaf-tune-bbb-e2e-v2-bug-cluster.md)
- [ADR-0543](0543-adr-0498-enforcement-hardening.md)
