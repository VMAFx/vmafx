- **`vmaf-tune fast` CLI surface (HP-3, ADR-0276 status update
  2026-05-08).** The `fast` subcommand is now reachable from the CLI,
  not just the Python API: `vmaf-tune fast --src ref.yuv --width
  1920 --height 1080 --target-vmaf 92.0` runs the proxy + Bayesian +
  GPU-verify pipeline end to end. The CLI is the single seam that
  injects the canonical-6 `sample_extractor` (probe encode + libvmaf
  per-feature JSON parse) and the real-encode `encode_runner` that
  `fast._build_prod_predictor` / `_gpu_verify` need — Python-API
  callers without those wired previously hit `NotImplementedError`,
  which made the changelog claim "production-wired" technically false
  at the CLI level. Output schema mirrors the JSON shape `recommend`
  and `predict` already emit (single source of truth) plus the
  fast-path-specific `verify_vmaf`, `proxy_verify_gap`, and
  `score_backend` diagnostics. Exit code `3` on out-of-distribution
  proxy/verify gaps is the documented fall-back signal in
  [`docs/usage/vmaf-tune.md`](docs/usage/vmaf-tune.md) §
  "Fall-back idiom" so a `vmaf-tune fast || vmaf-tune recommend`
  chain captures both error and OOD cases. `--smoke` keeps the
  synthetic CRF→VMAF curve so CI on hosts without ffmpeg / ONNX /
  GPU still exercises the search loop end to end. New tests in
  `tools/vmaf-tune/tests/test_cli_fast.py` (11 tests) cover the
  argparse wiring, smoke-mode payload schema, production seam
  injection, OOD exit-code contract, and the canonical-6 parser.
