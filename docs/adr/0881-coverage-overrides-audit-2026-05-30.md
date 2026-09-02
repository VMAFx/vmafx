# ADR-0881: Coverage-overrides audit — tighten template coverage floor

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: ci, coverage, dnn, gate, audit, adr-0114

## Context

[ADR-0114](0114-coverage-gate-per-file-overrides.md) introduced the
per-file critical-coverage override map in
`scripts/ci/coverage-check.sh` with two entries — `ort_backend.c` and
`dnn_api.c`, both at 78 %. A third entry,
`core/src/dnn/tiny_extractor_template.h` at 10 %, was added later when
the tiny-AI extractor refactor first landed and only one consumer
(`feature_lpips.c`) instantiated the inline helpers. The comment block
in the script gave the structural rationale: each new extractor uses a
different subset of the helper menu, so the floor must accommodate the
worst-case "very few helpers used" reality.

Time has passed. Four extractors now consume the template
(`feature_lpips.c`, `fastdvdnet_pre.c`, `feature_mobilesal.c`,
`feature_transnet_v2.c`), and the unit test suite exercises each. The
override has never been re-measured since the initial 10 % was set.

The motivating audit (digest:
[docs/research/0881-coverage-overrides-audit-2026-05-30.md](../research/0881-coverage-overrides-audit-2026-05-30.md))
ran a fresh `gcovr` pass against the meson unit suite under the same
build flags CI uses (`--buildtype=debug -Db_coverage=true
-Denable_float=true -Denable_avx512=true -Denable_dnn=enabled
-Dc_args=-fprofile-update=atomic`). The findings:

| File | Override (pre-audit) | Actual coverage | Slack |
| ------ | ---------------------- | ----------------- | ------- |
| `core/src/dnn/ort_backend.c` | 78 % | 77.8 % | -0.2 pp (at cap) |
| `core/src/dnn/dnn_api.c` | 78 % | 78.0 % | 0.0 pp (at cap) |
| `core/src/dnn/tiny_extractor_template.h` | 10 % | **77.4 %** | **67.4 pp** |

The first two entries' rationale still holds — both files hug the 78 %
ceiling described in ADR-0114. The third entry's rationale has been
overtaken by reality: with four extractors exercising the helpers, the
realistic floor is no longer 10 %.

Background: PR #338 (in flight) addresses the `ort_backend.c` cap with
a new test (`vmaf_ort_output_name_at`) that lifts coverage from 77.8 %
→ 78.5 %. That PR is the right way to handle an at-cap entry: add real
tests, do not relax the bar.

## Decision

Ratchet the `tiny_extractor_template.h` override from 10 % to 75 %, a
2.4 pp slack from the measured 77.4 % that mirrors the 1.3-1.7 pp slack
ADR-0114 used for the at-cap entries. Keep the other two overrides
unchanged. The override comment in `scripts/ci/coverage-check.sh`
documents the ratchet and back-references this ADR.

Codify a recurring rule for the per-file override map:

1. **Tighten when slack > 5 pp.** Override is silently giving up
   regression-lock; ratchet to `actual − 2 pp`.
2. **Keep at-cap entries.** Slack ≤ 2 pp = original ceiling rationale
   holds; leave alone.
3. **Remove when actual ≥ global 85 % floor.** Override redundant;
   delete and let the global gate enforce.
4. **Audit cadence: quarterly minimum, plus before any PR that
   modifies `PER_FILE_MIN`.** Audit procedure documented in the
   companion digest. ~5 min on a warm build tree.

The override map's "every entry must cite the ADR that justifies the
lower bar" rule (ADR-0114) is preserved — the `tiny_extractor_template.h`
comment block now cites both ADR-0114 (original rationale) and ADR-0881
(this audit's ratchet).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Tighten to 77 % (1 pp slack) | Tightest possible regression-lock | Real-world coverage flap risk on minor refactors; ADR-0114 deliberately chose 1.3-1.7 pp for the same reason | Match ADR-0114's risk tolerance — 2.4 pp |
| Tighten to 70 % (5 pp slack) | More comfortable headroom | Lets ~5 pp of real coverage slide back silently before tripping | Defeats the "lock the gain" goal |
| Remove the entry entirely (let global 85 % apply) | One fewer override; cleaner map | Actual is 77.4 %, below 85 % — would trip immediately. Either need ~8 pp of new tests or accept the structural ceiling rationale still holds at a tighter point | Premature; ratchet first, remove later only if helpers reach 85 % |
| Add fault-injection tests on the helpers to reach 85 % | Eliminates the override | The helpers are static inline conditionally instantiated per-extractor; injecting faults requires per-extractor wrappers. Effort ≫ benefit on a refactor template | Defer; revisit when a 5th extractor lands |
| Leave at 10 % "to be safe" | Zero risk of CI flap | Loses 67 pp of de-facto regression-lock; future drops from 77 % → 11 % would pass the gate silently | Strictly worse — the whole point of the gate is to catch silent regressions |

## Consequences

- **Positive**:
  - 65 pp of de-facto coverage now under regression-lock on
    `tiny_extractor_template.h`. Any drop from 77 % to 75 % trips the
    gate visibly; previously it would have slid to 11 % silently.
  - Audit procedure documented and codified into a recurring rule —
    next contributor knows when to tighten / keep / remove / add an
    override.
  - Other dnn/ files (`dnn_attach_api.c`, `model_loader.c`,
    `onnx_scan.c`, `op_allowlist.c`, `tensor_io.c`) plus `opt.c` and
    `read_json_model.c` confirmed clearing the global 85 % floor — no
    new overrides required.

- **Negative**:
  - 2.4 pp slack means a real ~2 pp drop in template coverage would
    pass silently. Smaller than the 67 pp blind spot we had before; a
    deliberate trade-off mirroring ADR-0114's risk tolerance.
  - If the build flags drift (`-Denable_dnn=enabled` defaulting back
    off, or ORT dropping from CI) the template's coverage could drop
    sharply and trip the gate. Mitigation: the script prints the
    applied threshold per file so the next CI run shows the regression
    cause immediately.

- **Neutral / follow-ups**:
  - When PR #338 lands, `ort_backend.c` will be at 78.5 %. Do **not**
    tighten the override to 78.5 % — 0.5 pp slack is too tight and
    would flap. Audit again at the next quarterly cycle.
  - When (if) `dnn_api.c`'s dead `has_norm` branch (lines 141-144 per
    ADR-0114 §Decision) is deleted, that file's coverage will jump
    ~3 pp to ~81 %. Re-audit then.
  - `core/src/dnn/tiny_extractor_template.h` is a header instantiated
    inline per-extractor; gcov sees it under the same path as the
    instantiating .c file. The 77.4 % number is the gcovr-reported
    line coverage; the `--filter 'src/.*'` argument keeps it in scope.
    Verified by inspecting the gcovr `.json` output.

## References

- [ADR-0114](0114-coverage-gate-per-file-overrides.md) — original
  per-file override design and at-cap rationale for `ort_backend.c` +
  `dnn_api.c`. This ADR is a follow-up audit, not a supersession.
- [ADR-0110](0110-coverage-gate-fprofile-update-atomic.md) — atomic
  profile updates that make the per-file numbers honest under
  parallel test runs.
- [ADR-0111](0111-coverage-gate-gcovr-with-ort.md) — gcovr (vs lcov)
  migration; ensures the per-file numbers are deduped.
- PR #338 (`fix/coverage-gate-bbcaa8d127-v2`) — in-flight fix that
  pushes `ort_backend.c` from 77.8 % → 78.5 % by adding a unit test
  on `vmaf_ort_output_name_at`. Demonstrates the "add tests, do not
  lower the bar" pattern this audit reinforces.
- Coverage audit research digest
  (`docs/research/0881-coverage-overrides-audit-2026-05-30.md`) —
  full audit digest including the per-file CSV, the reusable audit
  procedure, and the gate-output before/after diff.
- Source: `req` (paraphrased) — user directed "audit per-file overrides
  for stale entries that have been improved + identify new files that
  should have overrides … preserve the rule 'never lower a threshold
  to bypass' … only tighten or add tests."
