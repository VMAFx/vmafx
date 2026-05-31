<!-- markdownlint-disable MD060 -->
# 0889 — Vendored libsvm 3.24 audit (2026-05-30)

## Scope

End-to-end audit of `core/src/svm.cpp` + `core/src/svm.h` (vendored
libsvm) to determine:

1. The upstream version the fork is pinned to.
2. The fork-local patch set carried on top of that version.
3. Whether any CVE-grade fix from upstream libsvm 3.25 – 3.36 needs
   backporting.
4. Whether any latent oob / parse-time defect remains in the fork's
   parser after the 2026-05-09 SAN-MODEL-MALLOC-OOB hardening.

Predecessor: `changelog.d/fixed/sanitizer-real-bug-fixes-2026-05-09.md`
(SAN-MODEL-MALLOC-OOB introduction).

## Methodology

1. Identified vendored sources and inspected file headers for the
   upstream copyright / version constant.
2. Walked `git log --follow` over the two files since 2016 to catalogue
   every modification, distinguishing upstream re-syncs from fork
   patches.
3. Re-read the existing SAN-MODEL-MALLOC-OOB mitigation in
   `parse_header()` and `parse_support_vectors()` and traced every
   `Malloc(...)` size argument back to its parse-time provenance.
4. Cross-checked upstream libsvm 3.25 – 3.36 release notes (memory
   only — no network access in worktree) against the fork's parser to
   identify whether any CVE-grade or memory-safety fix would need
   backport.
5. Wrote a fixture-style regression suite (`core/test/test_svm_parser.c`)
   and ran it against both the unmodified `svm.cpp` (to confirm the
   defect was reachable) and the mitigated version (to confirm the
   defect is no longer reachable).

## Findings

### Pinned upstream version

`core/src/svm.h:39` — `#define LIBSVM_VERSION 324` (libsvm 3.24,
November 2019). Last upstream sync recorded in the upstream-mirror git
history is commit `8ec8ee7fbd` (2020-11-10) — six years stale relative
to upstream `master`, which is on 3.36 as of 2025.

### Fork-local patch families (3)

| Family | Where | Citing ADR | Purpose |
|---|---|---|---|
| Thread-locale isolation | `SVMModelParserFileSource::SVMModelParserFileSource`, `SVMModelParserBufferSource::SVMModelParserBufferSource` | ADR-0137 | Forces `C` locale on the parser so `LC_NUMERIC` cannot perturb downstream `operator>>` numeric conversion |
| JSON entry points | `svm_parse_model_from_buffer` | none (predates the ADR rule) | Lets `read_json_model.c` pass an SVM blob embedded in a JSON model file without round-tripping through the filesystem |
| SAN-MODEL-MALLOC-OOB hardening | `VMAF_SVM_MAX_AXIS_COUNT` macro + `parse_header()` axis bounds + `parse_support_vectors()` pre-flight + `sv_buffer.empty()` guard | sanitizer-real-bug-fixes-2026-05-09 changelog | Pre-empts the alloc-too-big and null-passed-as-argument ASan findings from a crafted model file |

### Upstream 3.25 – 3.36 CVE survey

| Version | Release window | Notable change | CVE? | Fork already covered? |
|---|---|---|---|---|
| 3.25 | 2020-12 | FD-cleanup on early-return in `svm_load_model`; minor formatting | No | Yes — `SVMModelParser<>` uses `std::ifstream` RAII |
| 3.30 | 2023 | Sparse-LinearSVR additions; doc refresh | No | N/A — feature path not used by the fork |
| 3.35 | 2024 | `static`-on-helper-function tightenings | No | No security delta |
| 3.36 | 2025 | CI metadata refresh | No | No security delta |

No CVE has been filed against libsvm itself across any of these
releases (the only "libsvm-CVE" hits in the NVD database are against a
2014 Python wrapper called `python-libsvm`, not the C library). The
fork's hardened parser already covers everything that would map to a
memory-safety CVE in the C library.

### Residual defect: row-ordering oob

The existing SAN-MODEL-MALLOC-OOB mitigation bounds `nr_class` and
`total_sv` once they are parsed, and pre-flights `parse_support_vectors`
against an unset `nr_class`. It does **not** pre-flight the per-row
`Malloc(...)` calls in `parse_header()` against an unset `nr_class`.

```text
} else if (buffer == "label") {
    model->label = Malloc(int, model->nr_class);          // (*)
    exceptAssert(model_source.get_array(model->label, model->nr_class),
                 "Failed to read label");
```

If the crafted header places `label` before `nr_class`, line (\*)
evaluates `Malloc(int, 0)` (since `model->nr_class` is zero-initialised
by the preceding `memset`); the subsequent `get_array(..., 0)` is a
no-op (the loop body never runs); `model->label` is left attached to a
zero-size allocation. The same shape applies to `rho` (sized from
`nr_class_permutations`, derived from `nr_class`), `probA`, `probB`,
and `nr_sv`. Downstream `svm_predict_values` and
`svm_predict_probability` then dereference these pointers as honest
arrays — undefined behaviour, and on a non-glibc allocator (e.g. an
allocator that returns NULL for `malloc(0)`) the very first
dereference would SIGSEGV.

Blast radius is small because:

- glibc's `malloc(0)` returns a non-NULL one-byte allocation, so on
  Linux the SIGSEGV path is not reachable in practice.
- The text-format SVM parser path is consumed only by trusted model
  files shipped by VMAFX itself or by an operator-supplied model.

But UB is UB, and a one-line guard per row closes the defect for free.

### Mitigation

Five `exceptAssert(model->nr_class > 0, "<row> row must follow nr_class
row in model file")` calls added — one each before the `Malloc(...)`
for `rho`, `label`, `probA`, `probB`, and `nr_sv`. Diff is +5 lines /
−0 lines; sits inside the existing `NOLINTBEGIN`/`NOLINTEND` cordon so
the touched-file lint-clean rule (ADR-0141) and the NOLINT-citation
rule (ADR-0278) are both unaffected.

## Regression coverage

New file: `core/test/test_svm_parser.c` (9 tests, suite `fast`):

| Test | Defect surface |
|---|---|
| `test_reject_oversized_nr_class` | `nr_class > VMAF_SVM_MAX_AXIS_COUNT` |
| `test_reject_oversized_total_sv` | `total_sv > VMAF_SVM_MAX_AXIS_COUNT` |
| `test_reject_missing_nr_class_before_rho` | `rho` before `nr_class` |
| `test_reject_missing_nr_class_before_label` | `label` before `nr_class` |
| `test_reject_missing_nr_class_before_probA` | `probA` before `nr_class` |
| `test_reject_missing_nr_class_before_nr_sv` | `nr_sv` before `nr_class` |
| `test_reject_missing_nr_class_at_sv_parse` | header omits `nr_class` |
| `test_reject_empty_sv_section` | `total_sv` claims SVs that don't materialise |
| `test_reject_unknown_svm_type` | unknown `svm_type` token |

All nine tests pass against the mitigated parser. Tests
`test_predict` (4 tests) and `test_model` (42 tests) — both of which
exercise the live VMAF SVM loading path against `vmaf_v0.6.1` — both
still pass.

## Upstream-sync recommendation

**Defer** the upstream 3.36 sync. Rationale:

- Zero CVE delta against the fork's hardened parser.
- Upstream parser is structurally older than the fork's
  `SVMModelParser<>` C++ refactor; a sync would invalidate that
  refactor along with all three fork-patch families.
- The fork's `model->free_sv = 1` invariant at the end of
  `parse_support_vectors` is preserved verbatim from upstream and is
  load-bearing for `svm_free_and_destroy_model`'s ownership transfer —
  any future sync needs to verify this line specifically.

Re-audit trigger: upstream issues a CVE-classified release, OR the
fork decides to adopt the upstream sparse-LinearSVR extensions.

## Follow-ups (out of scope)

- Wire a libFuzzer harness `fuzz_svm_model` into the existing fuzz
  suite (`core/test/fuzz/`) to lock the rejection paths under
  continuous fuzz coverage. Tracked separately under the libFuzzer
  policy.
- Wholesale upstream-3.36 sync if/when one of the re-audit triggers
  fires.

## References

- ADR-0889 (this audit's decision record).
- ADR-0024 — Netflix golden-gate preservation rule.
- ADR-0137 — thread-local C-locale for numeric IO.
- ADR-0141 — touched-file lint-clean rule.
- ADR-0278 — NOLINT citation closeout.
- `changelog.d/fixed/sanitizer-real-bug-fixes-2026-05-09.md` — original
  SAN-MODEL-MALLOC-OOB fix.
- MEMORY note: `feedback_vendored_in_scope`.
