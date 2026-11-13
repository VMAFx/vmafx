---
paths:
  - scripts/gen_*_onnx.py
  - scripts/gen_ssimulacra2_eotf_lut.py
invariant: ONNX generators pin metadata for deterministic bytes; EOTF LUT shifts regression gate.
---
<!-- markdownlint-disable MD013 MD038 -->
# ONNX fixture and SSIMULACRA 2 LUT generators

## Rebase-sensitive invariants

### `gen_smoke_onnx.py` and `gen_*_onnx.py` are deterministic

Fixture-generation scripts must produce byte-identical output
on re-run. The placeholder-model generators (`mobilesal`,
`dists_sq`) live in `ai/scripts/` only; no copy returns here.
Shipped `model/tiny/smoke_v0.onnx` and
`model/tiny/mobilesal.onnx` checked-in, sha256-pinned in
`model/tiny/registry.json`, verified by C-side loader
(`core/src/dnn/model_loader.c`). Non-deterministic regen breaks
registry sha256 + smoke gate. **On rebase**: keep
`onnx.helper.make_model(..., producer_name=..., producer_version=...,
ir_version=...)` pinned at fixed values; do not let `onnx` minor
version drift change output bytes. Same lesson encoded
in `ai/AGENTS.md` for bisect-cache fixtures.

### `gen_ssimulacra2_eotf_lut.py` regen is a Netflix-golden-adjacent event

Generated LUT at `core/src/feature/ssimulacra2_eotf_lut.h`
removes runtime `libm powf` dependency from SSIMULACRA 2
hot path. `powf` varies ~1 ULP between glibc / musl / macOS
libSystem, compounding to ~2e-4 per-frame drift in pooled
score. **On rebase**: do not regenerate LUT casually — regen
changes SSIMULACRA 2 fork-added regression-gate values
in `python/test/ssimulacra2_test.py` (per ADR-0164). If LUT
needs change, justify in commit message, walk regression test.
