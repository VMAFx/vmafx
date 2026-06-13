- **chore(lint):** ADR-0278 cite-form closeout sweep — audited all 222
  `// NOLINT` / `// NOLINTNEXTLINE` / `// NOLINTBEGIN` suppressions in
  fork-touched `core/src/`, `core/test/`, and `core/tools/` and appended
  inline ADR citations to 16 sites whose preceding comment described the
  load-bearing invariant in prose without naming an ADR. Touched:
  8 Metal extractor registration symbols (`core/src/feature/metal/*.mm`
  cite ADR-0361 / ADR-0421 / ADR-0490 / ADR-0589 — the registry-linkage
  invariant inherited from the HIP / CUDA / SYCL pattern), 3 integer-ADM
  upstream-mirror kernel sites (ADR-0141 §2 upstream-parity), the
  `predict.c` enum-cast bitmask suppression, the vendored libsvm
  whole-file `NOLINTBEGIN`, the `output.c` writer-pattern block, and the
  two `test_iqa_convolve.c` test-scaffolding blocks. No behavioural
  change, no function bodies split, no NOLINTs removed. Skipped 18 sites
  in files owned by in-flight DRAFT PRs (`core/src/model.cpp` deleted by
  PR #205; `core/src/output.cpp` deleted by PR #205;
  `core/src/feature/sycl/integer_adm_sycl.cpp` and `integer_vif_sycl.cpp`
  touched by 19 sibling drafts) — those sites already carry per-block
  prose justification and the cite-form sweep will follow once the
  merge-train upstream settles. (ADR-0141 §2 / ADR-0278)
