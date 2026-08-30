- `docs/state.md` staleness sweep 2026-05-08. Bumped header date
  (2026-05-06 → 2026-05-08). Closed three rows that this session
  discovered were already shipped but `docs/state.md` never tracked
  the closure (CLAUDE.md §12 r13 reviewer-enforced rule, no CI gate
  yet — separate backlog row): (a) **T6-1 / Tiny-AI C1 baseline**
  `fr_regressor_v1.onnx` shipped via PR #249 (`f809ce09`,
  2026-05-02) with [ADR-0249](docs/adr/0249-fr-regressor-v1.md) +
  `docs/ai/models/fr_regressor_v1.md` already on master — moved
  from "Deferred (waiting on external dataset access)" to
  "Recently closed"; (b) **T6-2a-followup' / saliency replacement**
  delivered via path C (`saliency_student_v1`, ~113 K params
  trained from scratch on DUTS-TR, IoU 0.6558) shipped in PR #359
  (2026-05-05, [ADR-0286](docs/adr/0286-saliency-student-fork-trained-on-duts.md))
  — moved from "Deferred" to "Recently closed" with a note that
  path A (op-allowlist `Resize` decision) closed by
  [ADR-0258](docs/adr/0258-onnx-allowlist-resize.md) (Accepted
  2026-05-03, opted against per-attribute enforcement aligning
  with [ADR-0169](docs/adr/0169-wire-scanner-scope.md) wire-scanner-scope
  rule) and path B (u2netp upstream-mirror via fork release artefact)
  is in flight as PR #469. Added one new follow-up row in
  "Open bugs": **T-VK-VIF-1.4-RESIDUAL** tracking the
  `integer_vif_scale2` 45/48-frame `places=4` mismatch on
  NVIDIA-Vulkan that survives PR #346's Step A `precise`
  decorations — bisect needed to determine if the gap is the
  same f32-vs-f64 colour-chain class as T-VK-CIEDE-F32-F64 or a
  different contraction surface.
  Companion appendix lands on
  [ADR-0265](docs/adr/0265-u2netp-saliency-replacement-blocked.md)
  `### Status update 2026-05-08` (paths A + C closed; path B
  in flight at #469); ADR body unchanged per
  [ADR-0028](docs/adr/0028-adr-maintenance-rule.md) immutability
  rule. Coordinates with PR #455 (state.md audit-backfill, also
  draft); whichever lands first, the other rebases.
