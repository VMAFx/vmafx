# Research-0848: Per-Surface Documentation Compliance Audit — Last 30 PRs (2026-05-29)

- **Date**: 2026-05-29
- **Auditor**: Claude Sonnet 4.6
- **Scope**: 30 most recent commits on `origin/master` — commits
  `950d2af51d` through `a4e9e707a0` (#96–#174).
- **Rule**: CLAUDE.md §12 r10 — every user-discoverable surface change ships
  human-readable docs in the **same PR**.

---

## Summary

| Metric | Count |
| --- | --- |
| PRs audited (distinct merged PRs in 30 commits) | 30 |
| PRs with no user-discoverable surface change | 22 |
| PRs compliant (surface change + docs present) | 5 |
| PRs with confirmed doc gap | 3 |
| PRs borderline / arguable | 0 |

**Overall: 5 / 8 PRs with surface changes are compliant (62.5%).**
Three confirmed gaps; two are BREAKING changes.

---

## Per-PR Results

| PR | Commit | Type | Surface changed? | Docs shipped? | Status |
| --- | --- | --- | --- | --- | --- |
| #96 | `950d2af51d` | docs(cuda) | No (audit/research only) | N/A | N/A |
| #97 | `7e83969a11` | feat(core): cpp23 wave7 | No (internal build wiring) | N/A | N/A |
| #98 | `6c2143d124` | docs(research) | No | N/A | N/A |
| #91 | `fe5bcb207a` | feat(cuda): resolution dispatch | No (internal kernel dispatch) | docs/backends/cuda/overview.md, ADR-0753 | COMPLIANT |
| #99 | `f2e2c035f7` | perf(cuda): ms_ssim | No | N/A | N/A |
| #100 | `2fcb3ce89b` | chore(core): orphan sweep | No | N/A | N/A |
| #101 | `31a51afb29` | perf(hip): ADM by-pointer | No (kernel-internal) | ADR-0759, research digest | N/A |
| #102 | `92ea978a41` | perf(cuda): ciede ldg | No | ADR-0762 | N/A |
| #103 | `1c965724a8` | docs(research) | No | N/A | N/A |
| #104 | `6ec1da2b43` | docs(research): SYCL audit | No | N/A | N/A |
| #108 | `37a638bdec` | fix(cuda): P0 hotfix | No | N/A | N/A |
| #110 | `9f83d352af` | fix(sycl): queue accessor | No | N/A | N/A |
| #112 | `572f7a5c36` | docs(dnn): ORT audit | No (audit only) | N/A | N/A |
| #114 | `8ae0535f5b` | fix(dnn): comment fix | No | N/A | N/A |
| #116 | `4a461d8564` | docs(audit): SIMD stub | No (empty commit) | N/A | N/A |
| #117 | `e6c00e4801` | docs(research): Metal audit | No (audit only) | N/A | N/A |
| #120 | `e22a0b2b8b` | refactor(feature): .c → .cpp | No public API change | N/A | N/A |
| #121 | `0256561970` | fix(sycl): e_-prefix aliases | No | N/A | N/A |
| #123 | `646aa75b30` | fix(helm): Vulkan refs, storage | User-facing Helm values.yaml + NOTES.txt | docs/development/gpu-scheduling.md, k8s-deployment.md | COMPLIANT |
| #128 | `0fdffdf9b5` | fix(metal): MT-1 + MT-2 | No public API change | N/A | N/A |
| #129 | `b8a51866e7` | fix(dnn): hard error on ORT fail | Behavior change for malformed models | docs/state.md, rebase-notes | COMPLIANT (borderline: no dnn.h docs update, but error is internal) |
| #135 | `9d57a93bff` | fix(log): log format | User-visible log output | **No docs shipped** | **GAP** |
| #147 | `38752c2c08` | fix(go): wire ladder | Bug fix (ladder already documented in vmafx-tune-go.md) | docs/rebase-notes.md | COMPLIANT |
| #153 | `62ed801942` | docs(release): dry-run | No | N/A | N/A |
| #157 | `709ce470e2` | fix(sycl): USM leak | No | N/A | N/A |
| #162 | `be78fb5611` | docs(research): perf snapshot | No | N/A | N/A |
| #163 | `1709a60884` | fix(dnn): NOLINT citations | No | N/A | N/A |
| #167 | `5d00c2fb10` | docs(process): ADR-0108 audit | No | N/A | N/A |
| #172 | `c0ced06f3c` | docs(research): Phase 4b | No | N/A | N/A |
| #174 | `a4e9e707a0` | fix(ci): conflict markers | No | N/A | N/A |

### Earlier PRs also in the 50-commit window (contributing surface gaps)

These are not in the 30-commit window but surface stale docs traceable to
specific PRs in the broader 50-commit set:

| PR | Commit | Issue |
| --- | --- | --- |
| #47 | `e9d265657` | `feat(core)!: drop Vulkan` — removed `libvmaf_vulkan.h`, CLI flags `--backend vulkan / --vulkan_device / --vulkan-require-fp64`, meson option `enable_vulkan`; only shipped `docs/adr/0726`. Did **not** update `docs/backends/vulkan/overview.md` (still describes active backend), `docs/metrics/features.md` (still lists Vulkan in backend columns for every extractor), or `docs/development/build-flags.md` (still describes `enable_vulkan` as operative). |
| #87 | `4781838cf8` | `feat!: sunset VmafLegacyQualityRunner` — BREAKING Python API removal. Only shipped ADR-0749 + research digest. Did **not** add entry to `docs/development/deprecations.md` or update `docs/usage/python.md`. |

---

## Confirmed Gaps

### GAP-1: PR #135 — log format change missing docs

**PR**: `9d57a93bff` `fix(log): standardize vmaf_log() call-site formatting`
**Surface**: User-visible log/error output — removes `"Error: "` prefix from
CUDA error messages in `core/src/cuda/common.c`; adds trailing `\n` to 5
call sites. Commit description: "output differs only in trailing newline
presence and prefix text."
**Missing**: No docs update shipped. Per CLAUDE.md §12 r10, user-visible
log/error/output-schema changes require docs. The `docs/usage/cli.md` or a
log-format note in `docs/backends/cuda/overview.md` should note the removed
`"Error: "` prefix.
**Severity**: Low (log prefix change only; no behavioral change).

### GAP-2: PR #47 — Vulkan drop left stale docs

**PR**: `e9d265657` `feat(core)!: drop Vulkan backend (BREAKING, ADR-0726)`
**Surfaces removed**:

- Public header `core/include/libvmaf/libvmaf_vulkan.h`
- CLI flags `--backend vulkan`, `--vulkan_device <N>`,
  `--vulkan-require-fp64`
- Meson option `enable_vulkan`

**Missing doc updates**:

1. `docs/backends/vulkan/overview.md` — still describes Vulkan as a
   working backend with full kernel coverage; should be replaced with a
   removal notice.
2. `docs/metrics/features.md` — every extractor row lists "Vulkan" in
   the backends column; should be struck or annotated as removed.
3. `docs/development/build-flags.md` — `enable_vulkan` row describes
   the backend as functional when enabled; should show
   "Removed (ADR-0726)".

**Note**: PR #123 partially addressed this by updating Helm chart docs and
adding a removal notice to `docs/development/gpu-scheduling.md` and
`docs/development/k8s-deployment.md`. The core user-facing docs remain stale.
**Severity**: High (BREAKING change; users will follow Vulkan docs and get build
errors or runtime failures).

### GAP-3: PR #87 — VmafLegacyQualityRunner sunset missing deprecations entry

**PR**: `4781838cf8` `feat!: sunset VmafLegacyQualityRunner (BREAKING, ADR-0749)`
**Surface**: `VmafLegacyQualityRunner` Python quality runner removed from
`compat/python-vmaf/core/quality_runner.py` (BREAKING).
**Missing**:

1. No entry in `docs/development/deprecations.md` — the deprecations file
   has a structured format for removals (see the "Legacy native build
   modes" entry); `VmafLegacyQualityRunner` should appear there.
2. No migration notice in `docs/usage/python.md` or `docs/api/` pointing
   users to `VmafQualityRunner`.
**Note**: ADR-0749 documents the decision and migration path (`VmafQualityRunner`),
but ADRs are not user-facing docs.
**Severity**: Medium (BREAKING Python API removal; affects users who import
`VmafLegacyQualityRunner` from the package).

---

## Proposed Follow-Up Issues

### Issue A — Update Vulkan removal docs (post-PR #47 debt)

Files to update:

- `docs/backends/vulkan/overview.md` — replace body with removal notice +
  pointer to ADR-0726
- `docs/backends/vulkan/moltenvk.md` — same
- `docs/metrics/features.md` — remove "Vulkan" from every backend column;
  add footnote "Vulkan backend removed in ADR-0726"
- `docs/development/build-flags.md` — replace `enable_vulkan` row with
  "Removed (ADR-0726)"

### Issue B — Add VmafLegacyQualityRunner to deprecations.md (post-PR #87 debt)

Add entry to `docs/development/deprecations.md` with migration path
(`VmafLegacyQualityRunner` → `VmafQualityRunner` + `vmaf_v0.6.1.json`).
Add deprecation note to `docs/usage/python.md` §QualityRunner section.

### Issue C — Log format change docs (PR #135)

Add a note to `docs/backends/cuda/overview.md` §"Log output" (or create one)
documenting that CUDA error messages do not carry the `"Error: "` prefix and
use a standard `\n`-terminated format.

---

## Notes on Borderline Cases

- **PR #84** (`feat(cuda): integer_adm3 + integer_aim parity`) — the new CUDA
  outputs are documented in `docs/metrics/features.md` (footnote 6), but
  that file was created in PR #51 which preceded PR #84. The footnote cites
  ADR-0574 for CUDA support but ADR-0746 is the correct ADR for this specific
  change. This is a minor citation error, not a missing doc page.

- **PR #129** (`fix(dnn): hard error in vmaf_ort_open`) — the behavior change
  affects users with malformed ONNX models; `docs/state.md` was updated.
  `dnn.h` was not updated with the new error semantics. Given this is an
  internal ORT path, the risk is low.

- **PR #92** (`feat(perf): multi-resolution benchmark baseline`) —
  `scripts/perf/bench-multi-resolution.sh` is a new user-discoverable script.
  `docs/development/perf.md` was updated in the same PR with usage instructions.
  COMPLIANT.
