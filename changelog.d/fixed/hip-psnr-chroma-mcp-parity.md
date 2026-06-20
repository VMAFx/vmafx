- HIP `psnr_hip` now emits `psnr_cb` / `psnr_cr` again: the extractor advertised
  the chroma features but its `options[]` table was empty, so `enable_chroma`
  was stuck at its zero-init `false` and `n_planes` never reached 3 — chroma was
  never dispatched. Added the `enable_chroma` option (default `true`), matching
  the CPU/CUDA twins (ADR-0453/0471); the existing per-plane loops handle the
  rest.
- MCP Go/Python parity fixes (`cmd/vmafx-mcp/impl.go`): removed the dead
  `vulkan` backend value (ADR-0726 removed the backend; the `>=30→"vulkan"`
  payload-inference branch and the `_vulkan` symbol keyword produced a value the
  Python server never emits); `run_tune_per_shot` no longer passes `--format`
  to `vmaf-tune tune-per-shot` (that subcommand has no such flag — unlike
  `compare` — so every call argparse-errored); `vmaf_version` parsing aligned.
- MCP Python server (`server.py`): `probe_backend` with a missing `backend`
  argument now raises a `ValueError` instead of a raw `KeyError`.
