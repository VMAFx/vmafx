<!-- markdownlint-disable MD013 MD060 -->
# ADR-0889: Vendored libsvm 3.24 audit — close header-row-ordering oob, document upstream-version policy

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: vendored, security, libvmaf, fork-local

## Context

`core/src/svm.cpp` + `core/src/svm.h` are a verbatim vendored copy of
libsvm (Chih-Chung Chang / Chih-Jen Lin, BSD-3-Clause), pinned at
`LIBSVM_VERSION 324` (3.24, November 2019). The legacy text-format SVM
model loader (`svm_load_model` / `svm_parse_model_from_buffer`) is the
only execution path that consumes the vendored parser; it underpins
`predict.c` and the Netflix `vmaf_v0.6.1` golden-data pipeline.

The fork carries three documented patch families on top of upstream
3.24:

1. **Thread-locale isolation** ([ADR-0137](0137-thread-local-locale-for-numeric-io.md)) —
   the parser runs under a forced `C` locale (`buffer.imbue(...)`) so
   that downstream `operator>>` numeric conversions are not perturbed by
   the host's `LC_NUMERIC`.
2. **JSON entry points** — `svm_parse_model_from_buffer` accepts an
   in-memory SVM blob so the fork's `read_json_model.c` can pass through
   models embedded in JSON payloads.
3. **SAN-MODEL-MALLOC-OOB hardening** (sanitizer-real-bug-fixes-2026-05-09)
   — `VMAF_SVM_MAX_AXIS_COUNT` (1 << 24) bound on `nr_class` and
   `total_sv` plus `parse_support_vectors` pre-flight checks plus
   `sv_buffer.empty()` guard.

This ADR's audit found **one residual gap** in the SAN-MODEL-MALLOC-OOB
mitigation: rows for `rho`, `label`, `probA`, `probB`, and `nr_sv` size
their `Malloc(...)` argument from `model->nr_class` (or
`nr_class_permutations`, derived from it), but did not assert that
`nr_class` had already been parsed. A crafted model whose header places
any of those rows before the `nr_class` row would therefore allocate a
zero-size buffer and subsequently feed it into a no-op
`model_source.get_array(..., 0)`, leaving the corresponding pointer
attached to a zero-size allocation that downstream `svm_predict_values`
/ `svm_predict_probability` dereferences as an actual array. The defect
is small in blast radius (memory-safe under glibc since `malloc(0)`
returns a non-NULL one-byte allocation, but undefined behaviour and a
read-before-write on the predictor side), but the mitigation is also
small — a one-line `exceptAssert(model->nr_class > 0, ...)` per row.

The audit also looked at upstream libsvm 3.25 – 3.36 changelogs to see
if any CVE-grade fix needs backporting. **None of the upstream releases
since 3.24 are CVE-classified**, and the small parser tightening shipped
in 3.25 (anonymous file-pointer cleanup on early-return) is moot for
the fork: the fork's parser refactor (`SVMModelParserFileSource` /
`SVMModelParserBufferSource`) routes file lifetime through
`std::ifstream` RAII, which already cannot leak the FD on a throw. The
sustained recommendation is therefore *do not sync to upstream 3.36* —
the upstream churn would invalidate the three fork patches without
buying any security delta, and the `SVMModelParser<>` refactor is a
load-bearing improvement that upstream has not adopted.

## Decision

1. **Extend the SAN-MODEL-MALLOC-OOB guard** to every parser row whose
   `Malloc` size depends on `model->nr_class`: `rho`, `label`, `probA`,
   `probB`, `nr_sv`. Each row gains an `exceptAssert(model->nr_class > 0,
   "<row> row must follow nr_class row in model file")` before the
   `Malloc` call. This is a minimal, surgical change inside the
   existing `NOLINTBEGIN`-cordoned vendor block (per
   [ADR-0141](0141-touched-file-cleanup-rule.md), the cordon is the
   load-bearing invariant for upstream-diff reviewability — the new
   `exceptAssert` calls are fork-local additions that the `NOLINTBEGIN`
   block already covers).
2. **Add a fork-local regression suite** at
   `core/test/test_svm_parser.c` (wired into `meson test --suite=fast`)
   covering all nine malformed-input rejection paths the parser now
   enforces (two oversized-axis bounds, five missing-`nr_class`-before-X
   row-ordering paths, the missing-`nr_class`-at-SV-parse path, the
   empty-SV path, and the unknown-`svm_type` path). All assertions are
   `parser must return NULL` — no score-value assertions are added, so
   the golden-gate scope ([ADR-0024](0024-netflix-golden-preserved.md))
   is untouched.
3. **Defer the upstream 3.36 sync.** The audit found no CVE-grade fix
   in upstream 3.25 – 3.36 that the fork's hardened parser does not
   already cover, and the upstream FD-lifecycle refactor is moot under
   the `SVMModelParser<>` C++ refactor. Pin remains 3.24 +
   fork-patches; a re-audit gets scheduled when upstream issues a
   CVE-classified release or when the fork wants the upstream
   sparse-LinearSVR additions.
4. **Document the vendor policy** at `core/src/AGENTS.md` so future
   sync attempts cannot silently regress the three fork-patch
   invariants (thread-locale, JSON entry point, MALLOC-OOB hardening).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Sync to upstream libsvm 3.36 wholesale | Picks up two `static`-on-helper-function tightenings and a CI metadata refresh | Invalidates all three fork patches (locale, JSON entry, MALLOC-OOB hardening); upstream parser is structurally older than the fork's `SVMModelParser<>` refactor; no CVE delta to amortise the churn | Not chosen — pure churn risk, no security gain |
| Add the `nr_class > 0` guard only to `nr_sv` (the most-likely misordering) | Smaller diff | Leaves the read-before-write hole open on `rho`/`label`/`probA`/`probB`; partial fix is harder to reason about than the consistent one | Not chosen — uniformity of the guard is the load-bearing invariant |
| Move the entire libsvm vendor to a CMake subproject | Cleanest patch isolation | Requires Meson-vs-CMake glue, doesn't reduce diff vs. upstream | Not chosen — disproportionate scope for a one-line bug |
| Wrap the parser in a libFuzzer harness (`fuzz_svm_model`) | Catches future regressions automatically | Worth doing as a follow-up, but does not by itself close the present defect | Not chosen as the primary mitigation — the unit-test suite proves the rejection path today; a fuzz harness is a logical follow-up |

## Consequences

- **Positive**: the libsvm parser now uniformly rejects every malformed
  header that the upstream 3.24 baseline accepted into a downstream
  zero-size allocation. The new regression suite locks the rejection
  paths against silent re-introduction, including by a future upstream
  sync. The audit closes the vendored-code line item from the
  `feedback_vendored_in_scope` memory note for libsvm.
- **Negative**: one additional fork-patch family to carry on the next
  sync (the `nr_class > 0` guards). The cost is minor — five identical
  one-line asserts grouped under the same `SAN-MODEL-MALLOC-OOB`
  ADR-0889 citation comment.
- **Neutral / follow-ups**: optional — wire a libFuzzer harness
  (`fuzz_svm_model`) into the existing fuzz suite. The audit also
  surfaced that the `model->free_sv = 1` flag at the tail of
  `parse_support_vectors` (a vendor-original line preserved verbatim)
  is the load-bearing invariant for `svm_free_and_destroy_model`'s
  ownership transfer — documented in the new
  `core/src/AGENTS.md` so a future refactor does not accidentally
  flip it.

## References

- [ADR-0024](0024-netflix-golden-preserved.md) — Netflix golden-gate
  preservation rule (this audit adds no score-value assertions).
- [ADR-0137](0137-thread-local-locale-for-numeric-io.md) — thread-local
  C-locale forcing in the SVM text parser.
- [ADR-0141](0141-touched-file-cleanup-rule.md) — touched-file
  lint-clean rule; the `NOLINTBEGIN`/`NOLINTEND` cordon on `svm.cpp` is
  the load-bearing invariant the audit preserves.
- [ADR-0278](0278-nolint-citation-closeout.md) — NOLINT citation rule;
  the existing cordon comment is cited and not relaxed.
- [ADR-0683](0683-cjson-banned-function-remediation.md) — sibling
  vendored-code remediation precedent (cJSON).
- `changelog.d/fixed/sanitizer-real-bug-fixes-2026-05-09.md` — original
  SAN-MODEL-MALLOC-OOB fix this ADR extends.
- Source: `feedback_vendored_in_scope` (MEMORY.md) — vendored libsvm
  receives the same audit and fix treatment as fork-original code.
- Research digest: [docs/research/0889-libsvm-vendored-audit-2026-05-30.md](../research/0889-libsvm-vendored-audit-2026-05-30.md).
