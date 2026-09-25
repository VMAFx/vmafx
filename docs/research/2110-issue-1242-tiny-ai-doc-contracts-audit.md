<!-- markdownlint-disable MD013 -->
# Research-2110: Tiny-AI Documentation and Runtime Contract Audit for Issue #1242

- **Status**: Complete
- **Workstream**: Issue #1242 (Int8 static/QAT shipping readiness & docs/ai gaps), Epic #1246 (One-shot retrain)
- **Last updated**: 2026-09-25
- **Deciders**: Maintainer (audit and close non-training documentation/runtime-contract task of #1242)

## 1. Context and Objective

GitHub Issue #1242 ("Tiny-AI 1.0.0 shipping readiness") tracked several prerequisite tasks before the single retraining pass in Epic #1246. Among these was closing three live documentation and runtime contract gaps across `docs/ai/`:

1. **Sidecar quarantine** in `docs/ai/sidecar-online-training.md` (evaluating Research-0733 §3.4 against in-tree code).
2. **TransNet V2 sliding-window contract** in `docs/ai/extractor-template.md` (clarifying shipped `core/src/feature/transnet_v2.c` vs legacy "planned `feature_transnet_v2.c`").
3. **Self-hosted inference runner and cross-device parity** in `docs/ai/inference.md` (truth regarding GPU runner labels, CI workflows, and ungated multi-provider parity).

This investigation audits the repository code, ADRs, and workflows to verify factual state, remediate link defects, and establish executable regression contract tests.

---

## 2. Findings by Surface

### 2.1 Sidecar Quarantine (`docs/ai/sidecar-online-training.md`)

[Research-0733 §3.4](0733-vmafx-sidecar-training-architecture.md) proposed a stability gate, fixture set, three-way version policy, and node digest checks. An exhaustive audit reveals:

| §3.4 Proposal Item | In-Tree Status | Audited Reality |
| --- | --- | --- |
| Atomic checkpoint write | **Implemented** | `SGDEMATrainer.export_onnx` (`ai/sidecar/sgd_ema.py`) writes to `.tmp.onnx` and renames via `os.replace`. |
| `.sha256` sidecar write | **Implemented** | `_write_sha256_sidecar` (`ai/sidecar/online_trainer.py`) writes digest alongside ONNX. |
| Node digest verification | **Not written** | `cmd/vmafx-node/` contains zero SHA-256 verification logic; produced `.sha256` is unconsumed. |
| `status.modelVersion` propagation | **Disconnected** | Types exist in `api/vmafx/v1/` and controller, but controller expects HTTP trainer service not exposed by sidecar. |
| `spec.versionPolicy` field | **Absent** | Neither `vmafx.dev_vmafxmodeltrainings.yaml` nor Go struct `VmafxModelTrainingSpec` declares `versionPolicy`. |
| Stability gate fixture set | **Does not exist** | No 10–20 pair calibration set is packaged for online sidecar gating. |
| PLCC controller comparison | **Not written** | Controller contains no background PLCC scoring worker. |
| `unstable` tag / quarantine | **Not written** | Every committed checkpoint is immediately exposed; no withholding mechanism exists. |
| `stability_plcc_delta` knob | **Absent** | Not declared in CRDs, environment variables, or config structs. |
| Automatic rollback threshold | **Not written** | Automatic failure-rate rollback is a proposal only. |

**Conclusion**: `docs/ai/sidecar-online-training.md` correctly marks the entire quarantine and stability gate mechanism as unimplemented. Every checkpoint must be treated as an unvetted standalone artifact requiring external promotion.

### 2.2 TransNet V2 Sliding-Window Contract (`docs/ai/extractor-template.md`)

- **Naming & Registration**: The extractor is shipped as `core/src/feature/transnet_v2.c` (registered in `core/src/meson.build` and `core/src/feature/feature_extractor.cpp`).
- **Tensor I/O**:
  - Input: `[1, 100, 3, 27, 48]` (`"frames"`) float32.
  - Output: `[1, 100]` (`"boundary_logits"`) float32.
  - Provided features: `shot_boundary_probability` (sigmoid) and `shot_boundary` (binary threshold 0.5).
- **Execution Lifecycle**:
  - 100-slot ring buffer.
  - No decimation: runs on every `extract()` call (dominant per-frame cost).
  - Replicate-edge warm-up over initial ~50 frames.
  - Per-shot aggregation into intervals/CRF targets is deferred to backlog item T6-3b.
  - Returns `-ENOSYS` on `-Denable_dnn=false` builds.
- **Link Correction**: Relative link `../../docs/metrics/features.md` from `docs/ai/` repaired to `../metrics/features.md#transnet_v2--transnet-v2-shot-boundary-detector-tiny-ai-nr--single-input`.

### 2.3 Self-Hosted Inference Runner & Cross-Device Parity (`docs/ai/inference.md`)

- **CI Runner Reality**:
  - `sycl-parity.yml` is gated behind `SYCL_ARC_RUNNER_ENABLED` with label `sycl-arc`.
  - `tests-and-quality-gates.yml` is gated behind `GPU_COVERAGE_ENABLED` with label `gpu-full`.
  - ADR-1319 introduced hosted pre-probes so jobs fail closed rather than queuing indefinitely.
  - Neither lane runs tiny-AI multi-provider score diffing; the documented CPU/CUDA bounds (1e-4 FP32, 1e-2 FP16) are workstation measurements, tracked as open row `T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25` in `docs/state.md`.
- **Link Correction**: Fixed citation typo `[ADR-0332](../adr/0405-openvino-npu-ep-wiring.md)` to `[ADR-0405](../adr/0405-openvino-npu-ep-wiring.md)`.

---

## 3. Executable Regression Contracts

Two executable test suites pin these contracts against regressions:

1. `ai/tests/test_tiny_ai_doc_contracts.py` (11 unit tests):
   - `SidecarQuarantineDocContractTest`: validates section headings, §3.4 itemization, atomic export in `sgd_ema.py`, digest generation in `online_trainer.py`, and absence in node/CRD.
   - `ExtractorTemplateDocContractTest`: validates source presence, meson build registration, `feature_extractor.cpp` symbols, and tensor dimension declarations.
   - `InferenceRunnerDocContractTest`: validates ungated bounds disclosure, workflow variable/label synchronization, and `docs/state.md` ledger presence.
   - `MarkdownLinksResolutionTest`: validates that all relative markdown links across the three documents resolve to existing paths on disk.
2. `ai/sidecar/tests/test_quickstart_contract.py`:
   - Enhanced with `test_checkpoint_quarantine_documented_as_unimplemented` to ensure the sidecar package guards its own documentation.

---

## 4. Issue and Epic Status

With the documentation and runtime contracts verified and test-pinned:

- **Issue #1242 Tasks**: All non-training tasks are complete. Only the single retraining pass remains (`Train and test the tiny-AI models as part of the single retraining pass`), which belongs to Epic #1246.
- **State Ledger**: Closed `T-DOCS-AI-GAPS-1242-2026-09-25` under `## Recently closed` in `docs/state.md`.

---

## 5. Related Decisions and References

- Epics: #1242 (int8 shipping readiness), #1246 (one-shot retrain)
- ADRs: [ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md), [ADR-0100](../adr/0100-project-wide-doc-substance-rule.md), [ADR-0781](../adr/0781-sidecar-sgd-ema-online-trainer.md), [ADR-1319](../adr/1319-fail-closed-self-hosted-gpu-admission.md)
- Research: [Research-0733](0733-vmafx-sidecar-training-architecture.md), [Research-2029](2029-int8-static-qat-readiness.md)
- Tracking: `T-DOCS-AI-GAPS-1242-2026-09-25`, `T-TINY-AI-CROSS-DEVICE-PARITY-UNGATED-2026-09-25`
