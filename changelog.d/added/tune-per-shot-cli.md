- **`vmaf-tune tune-per-shot` CLI subcommand
  ([ADR-0276](../docs/adr/0276-vmaf-tune-phase-d.md)).**
  Wires the Phase-D per-shot tuner into the `vmaf-tune` CLI. The
  underlying [`per_shot.py`](../tools/vmaf-tune/src/vmaftune/per_shot.py)
  module landed earlier; this entry just exposes it as a runnable
  subcommand: detects shots via `vmaf-perShot` (TransNet V2 weights)
  with a single-shot fallback, drives a target-VMAF predicate per
  shot, and emits a JSON encoding plan + optional copy-paste shell
  script. Plus an `import json` fix for `_run_predict` that was
  silently broken on master, and an adapter-aware quality-range
  test (replaces the literal `[15, 40]` window so the test tracks
  whatever the libx264 adapter declares — currently `(0, 51)` per
  ADR-0306).
