- **Per-GPU-generation ULP calibration table for the cross-backend
  parity gate (T-GPU-ULP / ADR-0234).** New
  [`scripts/ci/gpu_ulp_calibration.yaml`](scripts/ci/gpu_ulp_calibration.yaml)
  maps a runtime GPU identifier (Research-0041 schema:
  `vulkan:0xVVVV:0xDDDD` / `cuda:M.m` / `sycl:0xVVVV:DRIVER`) to a
  per-feature absolute tolerance. Both
  [`scripts/ci/cross_backend_vif_diff.py`](scripts/ci/cross_backend_vif_diff.py)
  and
  [`scripts/ci/cross_backend_parity_gate.py`](scripts/ci/cross_backend_parity_gate.py)
  now accept `--gpu-id <runtime_id>` and `--calibration-table <path>`;
  when omitted, behaviour is identical to before (per-feature
  `FEATURE_TOLERANCE` defaults remain authoritative). Lookup picks
  the most-specific glob match (`vulkan:0x10005:*` for lavapipe;
  trailing `*` is supported). The hosted-CI lavapipe lane in
  [`.github/workflows/tests-and-quality-gates.yml`](.github/workflows/tests-and-quality-gates.yml)
  passes `--gpu-id "vulkan:0x10005:0x0"` so the gate's tolerance
  decisions are now per-arch annotated in the parity report's JSON
  + Markdown artefacts. Initial coverage: 1 calibrated row (Mesa
  lavapipe — tolerances match the gate's pre-existing
  `FEATURE_TOLERANCE` defaults so behaviour is unchanged) plus 11
  placeholder rows (NVIDIA Ampere / Turing / Ada / Hopper, AMD
  RDNA2 / RDNA3, Intel Arc Alchemist / Battlemage, generic Intel
  SYCL); placeholders are functional no-ops until a real-hardware
  corpus replaces their `features:` block. New unit test
  [`scripts/ci/test_calibration.py`](scripts/ci/test_calibration.py)
  (19 cases) covers the loader, glob semantics, specificity ranking,
  and the shipped-table round-trip. The ONNX calibration head and
  `--gpu-calibrated` CLI flag from ADR-0234's "Decision" §
  remain deferred to the follow-up PR `feat(ai):
  T7-GPU-ULP-CAL — calibration-head v0`. See
  [ADR-0234](docs/adr/0234-gpu-gen-ulp-calibration.md) (now
  Accepted),
  [Research-0041](docs/research/0041-gpu-gen-ulp-calibration.md),
  and the rebase-notes entry under
  [`docs/rebase-notes.md`](docs/rebase-notes.md).
