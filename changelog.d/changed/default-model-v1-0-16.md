- **BREAKING (scores): the default VMAF model is now `vmaf_v1.0.16_3d0h`.**
  Scoring without `--model` used `vmaf_v0.6.1`; it now uses the v1.0.16
  standard 1080p / 3H model, the direct counterpart to v0.6.1's training
  condition. **Every default score changes** — on the standard 576x324 pair the
  pooled VMAF moves from `76.667831` to `82.816059`. Anyone comparing against
  historical numbers must pin `--model version=vmaf_v0.6.1` or rebaseline. No
  API, flag or output schema changed, and no model was removed: `vmaf_v0.6.1`
  and every other model remain built in and selectable.
  The default score now includes the v1 feature family — `integer_aim`,
  `cambi` banding and `speed_chroma` — and no longer includes `vif_scale0..3`
  or `motion2`, which are v0.6.1-family features.
  The 4K ladder moves with it (`vmaf_4k_v0.6.1` → `vmaf_v1.0.16_1d5h_2160`) so
  scores stay comparable across the 2160p boundary.
  **NEG is unchanged and now names a different generation**: Netflix published
  no NEG counterpart to any `vmaf_v1.0.16_*` model, so `--neg` still scores with
  the v0.6.1 family. The Python mirror previously derived NEG as
  `DEFAULT_MODEL + "neg"`, which would have synthesised the nonexistent
  `vmaf_v1.0.16_3d0hneg`; it is now an independent constant.
  Upstream Netflix still defaults to `vmaf_v0.6.1` — this is a deliberate fork
  divergence, recorded in `docs/rebase-notes.md`.
  **No Netflix golden assertion value was changed.** The single golden test that
  covered the default now names `vmaf_v0.6.1` explicitly, which reproduces its
  previous invocation byte-for-byte; the coverage it gave up is replaced by a
  fork-added `python/test/default_model_test.py` that asserts which model the
  default resolves to without hardcoding any score. Golden gate: 271 passed,
  12 skipped, 0 failed. See ADR-1169.
