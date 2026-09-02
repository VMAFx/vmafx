# ADR-0882: Fuzz target audit — JSON model + DNN sidecar harness expansion

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris, Claude
- **Tags**: ci, security, fuzzing, dnn

## Context

[ADR-0270](0270-fuzzing-scaffold.md) landed the libFuzzer scaffold with
`fuzz_y4m_input`; [ADR-0311](0311-libfuzzer-harness-expansion.md)
expanded it with `fuzz_yuv_input` + `fuzz_cli_parse`.
[Research-0083](../research/0083-libfuzzer-harness-expansion-target-survey.md)
ranked the remaining parser surfaces and identified two deferred
targets — `fuzz_model_load` (rank #3, libvmaf SVM model JSON
parser) and `fuzz_sidecar` (rank #4, tiny-AI sidecar JSON loader)
— as the next highest risk-weighted coverage delta for the
smallest harness LOC. Both surfaces are attacker-reachable
(`-m path=…` / `--tiny-model path=…` from the CLI) and have
classic fuzz-amenable shapes (incremental tokeniser feeding
arithmetic-grown arrays for the SVM parser; hand-rolled
`strstr` / `strchr` walkers with per-key heap allocations for the
sidecar parser). Both targets were green-lit in Research-0083
but punted from the ADR-0311 PR to keep that change focused.

## Decision

We will ship `fuzz_json_model` (wraps
`vmaf_read_json_model_from_buffer` and the collection variant)
and `fuzz_dnn_sidecar` (wraps `vmaf_dnn_sidecar_load`), each
with a small seed corpus committed verbatim and wired into the
nightly `.github/workflows/fuzz.yml` matrix. The harnesses are
opt-in via `-Dfuzz=true` (clang-only) and pair with
AddressSanitizer + UBSan; the nightly runs each for 60 s
against the seed corpus and uploads any crash artefacts.
First-run findings are documented in `docs/state.md` per
[ADR-0404](0404-nightly-fuzz-triage-keep-gates.md) (keep gates
running, document real bugs, no `continue-on-error` silencing).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Ship both harnesses with full CI wiring** (chosen) | Closes the two deferred targets from Research-0083 in one PR; nightly coverage starts on next scheduled run; matches the precedent set by ADR-0311 | First run will fail nightly until the surfaced fork-local OOB in `vmaf_model_destroy` is fixed | The ADR-0404 policy explicitly chose CI-red-but-honest over CI-green-but-silent; the harness is doing its job |
| Ship the harnesses but mark them `continue-on-error` until backing fixes land | CI badge stays green during the triage window | Direct violation of `feedback_no_test_weakening` and [ADR-0404](0404-nightly-fuzz-triage-keep-gates.md); silences a working detector | Aligned with the user's standing rule; ADR-0404 already adjudicated this exact trade-off for `fuzz_y4m_input` |
| Defer `fuzz_dnn_sidecar` until ORT integration stabilises (only ship `fuzz_json_model`) | Smaller PR; fewer moving parts | Sidecar parser already ships in master; the `extract_string` / `extract_string_array` family is fork-local code with no upstream fuzz coverage; deferring further keeps a high-risk surface untested | Both targets were already audited and green-lit in Research-0083; further deferral is purely procedural |
| Onboard the whole stack to OSS-Fuzz instead of expanding the in-tree harness set | Continuous fuzz at OSS-Fuzz scale; canonical "full" Scorecard credit | Onboarding has its own ADR-level scope (build container, contact email, dictionary set, manual maintainer review) | Tracked separately under ADR-0270 §Alternatives — orthogonal to the in-tree harness expansion |

## Consequences

- **Positive**:
  - Two more attacker-reachable parsers gain continuous fuzz coverage
    on the nightly job.
  - The Scorecard `Fuzzing` check stays in the "partial" bucket but
    with broader matrix breadth visible to reviewers (5 harnesses vs 3).
  - The first run surfaced one real fork-local heap-buffer-overflow
    (`T-JSON-MODEL-SLOPES-FEATURE-CAP-OOB-2026-05-30` in
    `docs/state.md`), demonstrating the harness works as intended.
- **Negative**:
  - The nightly `fuzz_json_model` leg will fail until the slopes /
    `feature_cap` reconciliation lands in a follow-up PR. Per ADR-0404
    this is the expected behaviour for a working detector.
  - The link strategy compiles `core/src/read_json_model.c` +
    `pdjson.c` + `dict.c` + `log.c` + `dnn/model_loader.c` directly
    into the harness binaries (instead of linking against
    `libvmaf.so`) because libvmaf is built with `-fvisibility=hidden`
    (ADR-0379) and the relevant entry points are not `VMAF_EXPORT`ed.
    This mirrors the existing `test_model` + `test_model_loader`
    precedents in `core/test/meson.build` and
    `core/test/dnn/meson.build`. The harness build additionally
    requires `-Db_lto=false` because ASan + LTO bitcode discards
    module-dtor sections at link time on the larger source set.
- **Neutral / follow-ups**:
  - `core/test/fuzz/json_model_known_crashes/slopes_oob_destroy.bin`
    is committed for regression coverage after the fix lands. It is
    deliberately excluded from the nightly seed corpus per the
    precedent established by `y4m_input_known_crashes/`. The `.bin`
    extension (rather than `.json`) sidesteps the `pre-commit
    check-json` hook, which would otherwise refuse to commit a
    fuzzer-mutated payload that is intentionally not valid JSON.
  - `core/test/fuzz/README.md` is updated with the two new rows in
    the Targets table and a known-crash note for the json_model OOB.
  - `docs/research/0083-libfuzzer-harness-expansion-target-survey.md`
    rows #3 + #4 are now landed; rows #5–#7 (`fuzz_per_shot`,
    `fuzz_output`, `fuzz_dnn_load`) remain backlog.

## References

- [ADR-0270](0270-fuzzing-scaffold.md) — initial scaffold.
- [ADR-0311](0311-libfuzzer-harness-expansion.md) — yuv_input +
  cli_parse expansion (the direct predecessor).
- [ADR-0379](0379-libvmaf-symbol-visibility.md) —
  `-fvisibility=hidden` policy that forces source-level link for
  non-exported parsers.
- [ADR-0404](0404-nightly-fuzz-triage-keep-gates.md) —
  keep-gates-running policy when a harness surfaces a real bug.
- [Research-0083](../research/0083-libfuzzer-harness-expansion-target-survey.md)
  — target ranking that green-lit the deferred pair.
- [libFuzzer (LLVM)](https://llvm.org/docs/LibFuzzer.html).
- [OSSF Scorecard `Fuzzing` check](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
- Source: agent task directive 2026-05-30 — "Audit existing fuzz
  targets + add fuzzers for high-risk parsing surfaces that lack one."
  (paraphrased)
