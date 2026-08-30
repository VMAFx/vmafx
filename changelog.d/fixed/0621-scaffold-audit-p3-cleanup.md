- **P3-1** `scripts/dev/permutation_importance.py`: replace hardcoded `/home/kilian/dev/vmaf`
  repo path with `Path(__file__).resolve().parents[2]` so the script works on any checkout.
- **P3-2** `ai/scripts/*.py` (13 files): update `.workingdir2/<corpus>/` default corpus
  paths to `.corpus/<corpus>/` matching the ADR-0547 / PR #983 data migration.
- **P3-3** Four stale `@unittest.skip` rationales rewritten to precisely name the root
  cause (XML attribute ordering, numpy RNG drift, deprecated `vmaf_feature` binary, missing
  `fps`/`format` filter keys). False ADR-0326 citation corrected in `asset_test.py`.
- **P3-4** Inline `places=1` justification comments added to deterministic quality-runner
  tests explaining the libsvm int→float32 SVM-head rounding.
- **P3-5** `docs/ai/model-registry.md`: new "CI-only smoke fixtures" section documents the
  six `smoke: true` registry entries as CI-only test fixtures exempt from ADR-0042.
  `lpips_sq_v1` / `lpips_sq.md` name mismatch documented as a tracked cosmetic gap.
- **P3-6** `.semgrepignore`: exclude `core/src/mcp/3rdparty/cJSON/cJSON.c` to silence
  three upstream cJSON `TODO`/`FIXME` markers without diverging from upstream.
- **SD-1** `docs/state.md`: add Open-bugs row for `T-VULKAN-MOTION-LAVAPIPE-INIT` (Vulkan
  motion / motion_v2 advisory CI gate) with a stated closure condition.
- **SD-2** `docs/state.md`: close `T-PYTHON-PERMUTATION-IMPORTANCE-HARDCODED-PATH` (fixed
  by P3-1 above). (ADR-0621)
