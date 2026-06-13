<!-- markdownlint-disable MD060 -->
# NOLINT inventory audit — ADR-0278 cite-form drift sweep (2026-05-30)

## Question

[ADR-0278](../adr/0278-t7-5-nolint-sweep.md) (Accepted 2026-05-04)
closed the T7-5 sweep with the claim **"75 sites, 0 missing ADR or
Research-digest references"** for `NOLINT(readability-function-size)`.
Three weeks later, has that property held for the broader NOLINT
surface (every check, not just `readability-function-size`)?

Trigger: [CLAUDE.md §12 r12](../../CLAUDE.md) requires every NOLINT in
tree to carry an inline ADR / research-digest / rebase-invariant
citation. Drift accumulates as new fork-touched code lands.

## Method

1. Worktree `chore/nolint-inventory-audit` cut from `origin/master` at
   `bbcaa8d1`.
2. `grep -rn -E '// NOLINT(NEXTLINE|BEGIN)?' core/src core/test
   core/tools` excluding `feature/third_party`, `feature/iqa/_iqa`, and
   `mcp/3rdparty/cJSON` (vendored / scoped block-cited at file head).
3. Python classifier: for each NOLINT site, read the preceding 6-line
   window plus the same line, search for any of ~80 documented
   justification markers (ADR-NNNN, Research-NNNN, "upstream-verbatim",
   "C ABI", "atomic_ref", "registry pattern", "extern linkage", "byte-
   exact", "vendored libsvm", "POSIX feature-test macro", etc.). The
   marker list is the union of every justification class observed in
   the already-cited NOLINTs across the tree.
4. For sites that fell out of the classifier, manual context-read of
   ±10 lines to distinguish (a) regex-miss false positives where the
   ADR is cited two lines further up than the window, from (b) genuine
   citation gaps that drifted in after ADR-0278.

## Findings

- **Total NOLINT sites in scope**: 222 (NOLINTNEXTLINE / NOLINTBEGIN
  / inline NOLINT — NOLINTEND markers excluded).
- **Cited automatically**: 187 sites passed the regex pass on first
  run.
- **Manual triage of the 35 remaining**: 18 turned out to be
  classifier false positives — ADR / "C ABI" / "extern linkage"
  citations existed but more than 6 lines above the NOLINT (long
  Doxygen blocks on `init` / `integer_compute_adm`).
- **Genuine ADR-form gaps**: 17 sites required inline ADR cite, of
  which 1 lives in `core/src/output.cpp` deleted by in-flight PR #205
  (skipped — owner conflict) — leaving 16 sites this PR actually
  edits.
- **In-flight-PR-owned (skipped)**: 18 NOLINTs total across
  `core/src/model.cpp` (16, file deleted by PR #205),
  `core/src/output.cpp` (1, file deleted by PR #205), and
  `core/src/feature/sycl/integer_adm_sycl.cpp` + `integer_vif_sycl.cpp`
  (1 each, both files touched by 19 sibling drafts). Each already
  carries per-block prose justification; the cite-form sweep on those
  files will follow once the merge-train upstream settles.

### Sites edited

| File | Lines edited | Cite added |
|---|---|---|
| `core/src/predict.c` | 497 | ADR-0278 (bitmask enum cast) |
| `core/src/svm.cpp` | 31 | ADR-0141 / ADR-0278 (vendored libsvm) |
| `core/src/output.c` | 80 | ADR-0141 / ADR-0278 (writer ferror pattern) |
| `core/test/test_iqa_convolve.c` | 99, 197 | ADR-0141 / ADR-0278 (test scaffolding) |
| `core/src/feature/integer_adm.c` | 2870, 3069, 3323 | ADR-0141 / ADR-0278 (upstream-mirror) |
| `core/src/feature/metal/float_psnr_metal.mm` | 253 | ADR-0361 / ADR-0278 |
| `core/src/feature/metal/integer_psnr_metal.mm` | 313 | ADR-0361 / ADR-0278 |
| `core/src/feature/metal/float_motion_metal.mm` | 333 | ADR-0361 / ADR-0278 |
| `core/src/feature/metal/float_ms_ssim_metal.mm` | 551 | ADR-0361 / ADR-0490 / ADR-0278 |
| `core/src/feature/metal/integer_motion_v2_metal.mm` | 439 | ADR-0421 / ADR-0278 |
| `core/src/feature/metal/float_moment_metal.mm` | 264 | ADR-0361 / ADR-0278 |
| `core/src/feature/metal/integer_motion_metal.mm` | 322 | ADR-0361 / ADR-0421 / ADR-0278 |
| `core/src/feature/metal/float_ssim_metal.mm` | 476 | ADR-0361 / ADR-0589 / ADR-0278 |

### Post-edit verification

Re-running the classifier with the same widened marker set against
the modified tree shows:

- **Cited**: 204 / 222 (was 187 pre-edit).
- **Uncited**: 18 sites — all 18 are in files owned by in-flight
  DRAFT PRs (15 in `model.cpp`, 1 in `output.cpp`, 1 in each of the
  two sycl files). All carry per-block prose justification; the
  citation form will be applied once the owning PRs land or are
  closed.

## Recommendation

- Land this PR (closes the 16 actionable cite-form gaps).
- After PR #205 (drops inert `.cpp` shadows) lands, run the audit
  again — the 16 `model.cpp` sites will be gone; the 1 `output.cpp`
  site will be gone.
- After the sycl-touching drafts settle (~19 PRs queued), open a
  cite-only sweep PR for `integer_adm_sycl.cpp:55` and
  `integer_vif_sycl.cpp:57`.
- Future enforcement (queued backlog item T7-5b per ADR-0278): a
  `scripts/ci/check-nolint-citation.sh` that runs the classifier in
  CI and gates new NOLINTs lacking an inline ADR token.

## References

- [ADR-0278](../adr/0278-t7-5-nolint-sweep.md) — T7-5 NOLINT-sweep
  closeout (parent cite-form policy).
- [ADR-0141](../adr/0141-touched-file-cleanup-rule.md) §2 — every
  NOLINT must cite the ADR / research-digest / rebase invariant.
- [CLAUDE.md §12 r12](../../CLAUDE.md) — touched-file lint-clean rule.
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — six
  deliverables (this digest is deliverable #1).
- Audit script (one-shot): the regex + manual-triage Python from this
  research digest (not committed; rerun with `grep -rn` +
  classifier).
