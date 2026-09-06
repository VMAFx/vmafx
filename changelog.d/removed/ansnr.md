- `ansnr` / `float_ansnr`: cleanly finalize removal of the legacy ANSNR feature
  extractor (ADR-0865). Scrubbed dead entries from CI parity tooling
  (`cross_backend_parity_gate.py`, `cross_backend_vif_diff.py`, `gpu_ulp_calibration.yaml`),
  recorded deprecation row in `docs/development/deprecations.md`, added rebase invariant
  in `core/src/feature/AGENTS.md`, and documented upstream re-drop procedure in `docs/rebase-notes.md`.
