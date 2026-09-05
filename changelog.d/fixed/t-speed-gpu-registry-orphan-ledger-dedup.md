- Bug ledger: de-duplicated the `T-SPEED-GPU-REGISTRY-ORPHAN-2026-06-19`
  entry in `docs/state.md`. The bug — the six GPU SpEED registrations
  (`speed_{chroma,temporal}_{cuda,sycl,hip}`) stranded in the dead
  `core/src/feature/feature_extractor.c` twin — was fixed on master by
  commit `a0bf83c214` (PR #1004), but the ledger carried two "Recently
  closed" rows and two tombstone comments for it. Collapsed to a single
  row, re-verified against `origin/master` (externs and registry entries
  present inside their `HAVE_CUDA` / `HAVE_SYCL` / `HAVE_HIP` guards; the
  dead `.c` twin gone). No source change.
