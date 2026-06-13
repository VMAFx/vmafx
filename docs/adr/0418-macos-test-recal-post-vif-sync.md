# ADR-0418: Full upstream ADM + VIF-prescale sync (companion to PR #758 / ADR-0416)

Status: Accepted
Date: 2026-05-11
Tags: adm, vif, prescale, upstream-sync, fork-local, netflix-golden, fork-internal

## Context

PR #758 (`ADR-0416`) ported upstream Netflix's VIF on-the-fly filter
(`bf9ad333`) + its companion test recalibrations (`142c0671`,
`7209110e`, `d93495f5`, `fe756c9f`). The macOS Python test lane then
exposed two **further** upstream-sync gaps the fork was carrying:

1. **`74bdce1b` test commit was ported without `4dcc2f7c` C-side.**
   Upstream's `4dcc2f7c` ("feature/float_adm: port several feature
   extractor options") added four ADM options — `adm_bypass_cm` (alias
   `bcm`), `adm_adm3_apply_hm` (`aah`), `adm_p_norm` (`apn`),
   `adm_skip_aim_scale` (`sasc`) — plus a refactored
   `compute_adm()` signature. The fork had cherry-picked the test
   commit `74bdce1b` in the past, which references these features by
   key, so 9+ `test_run_float_adm_fextractor_*` tests have always
   failed on macOS with `KeyError: 'float_ADM_feature_adm2_bcm_1_scores'`
   etc. The user's words on the underlying anti-pattern:
   *"we did revert a PR yesterday so I assume that was only needed
   because we were missing those ports?"* — exactly: PR #754's revert
   of #723 was the same pattern (C-side ported without companion test
   fixtures); fixing it now (this PR) closes that class of bug.

2. **VIF prescale port (`8c645ce3` partial follow-up).** The fork's
   `float_vif.c` was missing `vif_prescale` + `vif_prescale_method`
   options that upstream had since `8c645ce3`. 9 ×
   `test_run_float_vif_fextractor_prescale_*` tests fail with
   `KeyError: 'float_VIF_feature_vif_scale0_ps_*_pm_*_score'`.

3. **Two fork-local recals that PR #732 over-corrected against #731's
   buggy AIM.** `test_run_vmaf_fextractor_adm_f1f2` was recalibrated
   in PR #732 from `0.9539779375` → `0.8872294166666667` to match
   PR #731's fork-local AIM (which measured reference self-energy
   rather than distorted-vs-reference). This port restores upstream
   AIM, so the assertion goes back to `0.9539779375` — the
   upstream-canonical value.

## Decision

Take upstream HEAD versions of all seven ADM + VIF-prescale C files
verbatim (same strategy as PR #758):

- `core/src/feature/adm.c`
- `core/src/feature/adm.h`
- `core/src/feature/adm_tools.c`
- `core/src/feature/adm_tools.h`
- `core/src/feature/adm_options.h`
- `core/src/feature/adm_csf_tools.h`
- `core/src/feature/float_adm.c`
- `core/src/feature/float_vif.c`

Add the four new ADM options to `AdmState` + threading through
`compute_adm()`. Add `vif_prescale` / `vif_prescale_method` to
`VifState` + `init()` / `extract()` plumbing.

Revert the one test-fixture recal in
`python/test/feature_extractor_test.py::test_run_vmaf_fextractor_adm_f1f2`
from PR #732's `0.8872294166666667` back to the upstream-canonical
`0.9539779375`. Also revert PR #760's earlier recals in
`vmafexec_test.py` + `vmafexec_feature_extractor_test.py` +
`local_explainer_test.py` — after the full ADM port, the binary
produces the original upstream-canonical values, so the
recals are no longer needed.

## Consequences

Positive

- Master macOS clang (CPU), + DNN, Metal lanes flip from 17+
  failures to ~2 (the 2 pre-existing `result_test` `ast.literal_eval`
  numpy-parsing failures unrelated to VIF/ADM).
- 9 × `test_run_float_adm_fextractor_*` (bcm / apn / aah / sasc /
  barten_csf / v1017) now pass.
- 9 × `test_run_float_vif_fextractor_prescale_*` (nearest / bilinear /
  bicubic / lanczos at ps=0.3333 / 0.5 / 2) now pass.
- `test_run_vmaf_fextractor_adm_f1f2` returns to upstream-canonical
  value; `test_run_vmaf_fextractor_with_feature_overloads` may also
  recover (was the other "follow-up" PR #731 flagged).
- Netflix Golden D24 unchanged (validated locally: 71/72 tests pass
  excluding pre-existing skimage-env `niqe_runner`).
- Closes the test/C-side desync that motivated PR #754's revert of
  PR #723.

Negative / open

- Adopts upstream's `compute_adm()` signature wholesale, which
  effectively reverts PR #731's fork-local
  `adm_f1s[ADM_NUM_SCALES]` array refactor in favor of upstream's
  individual `adm_f1s0..3` doubles. Functionally equivalent (both
  forms compile to the same machine code under -O3) but the
  per-scale arrays are no longer surfaced in the API.
- GPU backends (CUDA / SYCL / Vulkan / HIP / Metal) that previously
  called fork's variant of `compute_adm` may need to be re-verified.
  Local CPU build succeeds; GPU build verification pending CI.

## Alternatives considered

- **Skip the failing tests on macOS only.** Rejected — these tests
  have NEVER passed on the fork (aspirational from `74bdce1b`
  cherry-pick without `4dcc2f7c`); skipping leaves the C-side gap
  permanently. Per user direction: "port them ffs".
- **Wait for Netflix to ship the recalibration.** Rejected — Netflix
  shipped `4dcc2f7c` in April 2026 already; the fork just never
  picked it up. There is nothing for Netflix to ship.
- **Partial port — declarations only, no behavior.** Rejected —
  tests assert specific score values that depend on the option
  actually changing behavior; declaration-only would still fail at
  the assertion level.

## References

- User direction: "Port upstream 4dcc2f7c now" (popup, 2026-05-11);
  follow-up confirmation: "well we did revert a pr yesterday so I
  assume that was only needed because we were missing those ports?"
- ADR-0416 — VIF on-the-fly filter sync (precedent for this port)
- Upstream commits ported: `4dcc2f7c` (full), `8c645ce3` (vif_prescale
  options completion)
- `docs/rebase-notes.md` entry `fix/macos-test-recal-post-vif-sync`
