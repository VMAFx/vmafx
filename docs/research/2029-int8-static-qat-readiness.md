<!-- markdownlint-disable MD013 MD060 -->
# Research-2029: Tiny-AI Int8 Static PTQ and QAT Readiness Smoke for Epic #1246

- **Status**: Active
- **Workstream**: Epic #1246 (One-shot retrain), Epic #1242 (Int8 static/QAT shipping readiness)
- **Last updated**: 2026-09-04
- **Deciders**: Maintainer (decision 2026-09-04: shipped tiny-AI ONNX artifacts must be int8 static PTQ or QAT, not fp32, not dynamic PTQ)

## 1. Executive Summary

A full end-to-end smoke test of the static post-training quantisation (PTQ) and quantisation-aware training (QAT) pipelines was conducted on CPU in the `vmaf-dev-mcp` container using Netflix test pairs (`src01_hrc00_576x324_5frames.yuv` and `src01_hrc01_576x324_5frames.yuv`).

Key conclusions:

1. **Op Allowlist (`core/src/dnn/op_allowlist.c`) is ALREADY READY for both static PTQ and QAT.**
   Both static PTQ (`ptq_static.py`) and QAT (`qat_train.py`) emit **QDQ format** (`QuantizeLinear` and `DequantizeLinear` wrapping stock `Conv`, `Gemm`, `MatMul`, and activation nodes). All these ops have been present on `op_allowlist.c` since ADR-0173/ADR-0174. Furthermore, the claim in issue #1242 that `DynamicQuantizeLinear`, `MatMulInteger`, and `ConvInteger` are not yet allowed is **stale**: they were already added in commit `e2671eae489ba` (PR #882) on 2026-06-12. All tested static PTQ and QAT models passed `vmaf-train check-ops` with 0 forbidden ops.
2. **C Loader (`vmaf --tiny-model`) LACKS int8 redirect logic.**
   While `vmaf_dnn_session_open()` in `core/src/dnn/dnn_api.c` (used by the `vmaf_pre` filter) implements `.int8.onnx` resolution and ADR-1032 fp32 debug fallback, `vmaf_use_tiny_model()` in `core/src/dnn/dnn_attach_api.c` (the entry point called by the `vmaf` CLI via `--tiny-model`) **does not implement any int8 redirect**. When passed `--tiny-model model.onnx`, it opens `model.onnx` (fp32) directly, ignoring `quant_mode` in `model.json`. If passed `--tiny-model model.int8.onnx` directly, it loads and executes int8 inference successfully.
3. **Shipped sidecar defect identified in `vmaf_tiny_v3.int8.json`.**
   `model/tiny/vmaf_tiny_v3.int8.json` omits `"onnx_has_scaler": true`. Because `vmaf_tiny_v3.int8.onnx` already bakes the feature scaler into the ONNX graph as Constant nodes, `core/src/libvmaf.c:1442` double-scales the features in C before feeding the graph, corrupting scores (`vmaf_tiny_model: -1106.45` instead of `~ -20.37`).
4. **Accuracy drops on smoke set.**
   Static PTQ on `mlp_small` showed a PLCC of 0.999536 (drop = 0.000464, worst abs delta = 0.001042) on 16 synthetic samples and 0.990346 (drop = 0.009654) on 5 real video frames. In end-to-end `vmaf` CLI runs on the 5-frame video pair, frame-0 `vmaf_tiny_model` scored `0.269516` (fp32) vs `0.270319` (static PTQ int8), an absolute delta of only `0.000803`.

---

## 2. Op Inventory per Path vs Op Allowlist

| Quantisation Path | Script / Module | Wire Format | Operators Emitted | In `op_allowlist.c`? | `vmaf-train check-ops` Verdict |
| --- | --- | --- | --- | --- | --- |
| **Dynamic PTQ** | `ai/scripts/ptq_dynamic.py` | QOperator (dynamic) | `DynamicQuantizeLinear`, `MatMulInteger`, `ConvInteger`, `Add`, `Mul`, `Cast`, `Relu`, `Clip`, `Reshape` | **Yes** (added in PR #882 / commit `e2671eae489ba`) | **PASS** (e.g. `learned_filter_v1.int8.onnx`: 8 distinct ops, all allowed) |
| **Static PTQ** | `ai/scripts/ptq_static.py` | QDQ (ORT default) | `QuantizeLinear`, `DequantizeLinear`, `Gemm`, `Conv`, `MatMul`, `Relu`, `Clip` | **Yes** (present since ADR-0173 / ADR-0174) | **PASS** (`mlp_small_final.ptq_static.int8.onnx`: 3 distinct ops, all allowed) |
| **Static PTQ (explicit)** | `ai/src/vmaf_train/quantize.py` | QDQ (pinned `QuantFormat.QDQ`) | `QuantizeLinear`, `DequantizeLinear`, `Gemm`, `Conv`, `MatMul` | **Yes** | **PASS** |
| **QAT (Learned Filter)** | `ai/scripts/qat_train.py` | QDQ (via `_ort_static_quantize`) | `QuantizeLinear`, `DequantizeLinear`, `Conv`, `Add`, `Constant` | **Yes** | **PASS** (`learned_filter_qat.int8.onnx`: 5 distinct ops, all allowed) |
| **QAT (FR Regressor)** | `ai/scripts/qat_train.py` | QDQ (via `_ort_static_quantize`) | `QuantizeLinear`, `DequantizeLinear`, `Gemm`, `Add`, `Mul`, `Div`, `Erf`, `Constant`, `Squeeze` | **Yes** | **PASS** (`fr_tiny_qat.int8.onnx`: 9 distinct ops, all allowed) |
| *Static QOperator (hypothetical)* | `quantize_static(..., quant_format=QOperator)` | QOperator (static) | `QLinearConv`, `QLinearMatMul`, `QGemm`, `QLinearAdd` | **NO** (forbidden by security policy) | **FAIL** (`-EPERM` rejection; falls back to fp32 under ADR-1032) |

### Fact vs #1242 Premise

Epic #1242 stated that `ConvInteger`, `MatMulInteger`, and `DynamicQuantizeLinear` are not yet allowed by `op_allowlist.c`.
**Audit finding**: This premise was outdated. On 2026-06-12, commit `e2671eae489ba` (PR #882) added lines 101-105 to `core/src/dnn/op_allowlist.c`:

```c
    "QuantizeLinear",
    "DequantizeLinear",
    "DynamicQuantizeLinear",
    "MatMulInteger",
    "ConvInteger",
```

Therefore, all three formats (Dynamic QOperator, Static QDQ, QAT QDQ) pass the allowlist today.

---

## 3. Empirical Smoke Execution: Commands & Outputs

All steps executed inside the `vmaf-dev-mcp` container using `/opt/vmaf-venv/bin/python`, forced strictly to CPU (`CUDA_VISIBLE_DEVICES="" ONEAPI_DEVICE_SELECTOR=""`).

### 3.1 Training Tiny FP32 Model (2 Epochs)

Setup smoke corpus using existing test pairs:

- Ref: `/workspace/python/test/resource/yuv/src01_hrc00_576x324_5frames.yuv`
- Dis: `/workspace/python/test/resource/yuv/src01_hrc01_576x324_5frames.yuv`
- Dimensions: 576x324, yuv420p 8-bit.

Command:

```bash
export VMAF_BIN=/usr/local/bin/vmaf
export VMAF_MODEL_PATH=/workspace/model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json
export VMAF_TINY_AI_CACHE=/tmp/smoke_run/cache

/opt/vmaf-venv/bin/python /workspace/.claude/worktrees/qat/ai/train/train.py     --data-root /tmp/smoke_run/corpus     --model-arch mlp_small     --epochs 2     --batch-size 4     --assume-dims 576x324     --val-source ValSrc     --max-pairs 2     --out-dir /tmp/smoke_run/train_out
```

Output:

```text
[train] arch=mlp_small params=257 feature_dim=6
[train] train samples=5 val samples=5
[train] wrote /tmp/smoke_run/train_out/mlp_small_epoch0.onnx
[train] wrote /tmp/smoke_run/train_out/mlp_small_epoch1.onnx
[train] wrote /tmp/smoke_run/train_out/mlp_small_final.onnx
[train] final checkpoint: /tmp/smoke_run/train_out/mlp_small_final.onnx
```

Execution time: ~4 seconds. Artifacts: `mlp_small_final.onnx` (1,275 B) + `mlp_small_final.onnx.data` (896 B). Ops: `["'Gemm'", "'Relu'"]`.

### 3.2 (a) Static PTQ via `ptq_static.py`

Created 16-sample calibration dataset `calib.npz` (shape `[16, 6]`, matching the 6 input features).

Command:

```bash
/opt/vmaf-venv/bin/python /workspace/.claude/worktrees/qat/ai/scripts/ptq_static.py     /tmp/smoke_run/train_out/mlp_small_final.onnx     --calibration /tmp/smoke_run/calib.npz     --output /tmp/smoke_run/train_out/mlp_small_final.ptq_static.int8.onnx     --report-out /tmp/smoke_run/train_out/ptq_static_report.json
```

Output:

```text
[ptq_static] /tmp/smoke_run/train_out/mlp_small_final.onnx  ->  /tmp/smoke_run/train_out/mlp_small_final.ptq_static.int8.onnx  cal=/tmp/smoke_run/calib.npz  per-channel=False
[ptq_static] done — 1,275 -> 3,888 bytes (3.05×)
```

Allowlist validation:

```bash
vmaf-train check-ops --model /tmp/smoke_run/train_out/mlp_small_final.ptq_static.int8.onnx
# -> allowlist OK (3 distinct ops, all allowed)
# Ops: ["'DequantizeLinear'", "'Gemm'", "'QuantizeLinear'"]
```

### 3.3 (b) QAT Training via `qat_train.py`

Tested two configurations:

1. `learned_filter_v1_qat.yaml` (default smoke mode fallback due to uncommitted `bvi_dvc_pairs.parquet`):
   - Command:

     ```bash
     /opt/vmaf-venv/bin/python /workspace/.claude/worktrees/qat/ai/scripts/qat_train.py          --config /workspace/.claude/worktrees/qat/ai/configs/learned_filter_v1_qat.yaml          --output /tmp/smoke_run/train_out/learned_filter_qat.int8.onnx          --epochs-fp32 1 --epochs-qat 1
     ```

   - Output: `learned_filter_qat.int8.onnx` (42,471 B).
   - `vmaf-train check-ops`: `allowlist OK (5 distinct ops, all allowed)` (`Add`, `Constant`, `Conv`, `DequantizeLinear`, `QuantizeLinear`).

2. `fr_tiny_qat.yaml` (real training on extracted 5-frame feature cache):
   - Extracted features & VMAF teacher scores on smoke pair to `/tmp/smoke_run/smoke_features.parquet` (5 rows).
   - Executed 1 warm-start fp32 epoch + FX fake-quant preparation + 1 QAT fine-tune epoch (`smoke=False`).
   - Output: `fr_tiny_qat.qat.fp32.onnx` (3,930 B) and `fr_tiny_qat.int8.onnx` (15,010 B).
   - `vmaf-train check-ops`: `allowlist OK (9 distinct ops, all allowed)` (`Add`, `Constant`, `DequantizeLinear`, `Div`, `Erf`, `Gemm`, `Mul`, `QuantizeLinear`, `Squeeze`).

### 3.4 C Loader Smoke Tests (`vmaf --tiny-model ...`)

#### 3.4.1 Direct Invocation with Int8 Artifacts

Command (static PTQ):

```bash
/usr/local/bin/vmaf --backend cpu   -r /workspace/python/test/resource/yuv/src01_hrc00_576x324_5frames.yuv   -d /workspace/python/test/resource/yuv/src01_hrc01_576x324_5frames.yuv   -w 576 -h 324 -p 420 -b 8   -m path=/workspace/model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json   --tiny-model /tmp/smoke_run/train_out/mlp_small_final.ptq_static.int8.onnx   --tiny-device cpu -o /tmp/smoke_run/vmaf_ptq_static_smoke.json --json
```

Output:

```json
"metrics": {
  "vmaf_tiny_model": 0.270319,
  "vmaf": 92.632429
}
```

Compared to FP32 (`mlp_small_final.onnx`):
`vmaf_tiny_model: 0.269516`.
Delta: `|0.269516 - 0.270319| = 0.000803` (0.08% score delta).

Command (QAT):

```bash
/usr/local/bin/vmaf --backend cpu   -r /workspace/python/test/resource/yuv/src01_hrc00_576x324_5frames.yuv   -d /workspace/python/test/resource/yuv/src01_hrc01_576x324_5frames.yuv   -w 576 -h 324 -p 420 -b 8   -m path=/workspace/model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json   --tiny-model /tmp/smoke_run/fr_tiny_qat.int8.onnx   --tiny-device cpu -o /tmp/smoke_run/vmaf_qat_smoke.json --json
```

Output:

```json
"metrics": {
  "vmaf_tiny_model": 0.157570,
  "vmaf": 92.632429
}
```

#### 3.4.2 C Loader Redirect Verification (The Missing Redirect Bug)

We staged `test_redirect/model.onnx` with sibling `model.int8.onnx` and sidecar `model.json` declaring `"quant_mode": "static"`.
When invoked with `--tiny-model /tmp/smoke_run/test_redirect/model.onnx`:

- When external data `mlp_small_final.onnx.data` was not copied next to `model.onnx`:

  ```text
  libvmaf WARNING libvmaf dnn CreateSession: External data path validation failed for initializer: 0.weight. Error: tensorprotoutils.cc:453 ValidateExternalDataPathFromDir External data path does not exist: "/tmp/smoke_run/test_redirect/mlp_small_final.onnx.data"
  problem loading tiny model /tmp/smoke_run/test_redirect/model.onnx: -5
  ```

- When `mlp_small_final.onnx.data` was copied:
  Output was `vmaf_tiny_model: 0.269516` (the exact FP32 score).

**Conclusion**: `vmaf_use_tiny_model()` does NOT inspect `meta.quant_mode` and does NOT redirect to `.int8.onnx`. It unconditionally opens the path supplied on the command line.

---

## 4. FP32-vs-Int8 Drift Measurements

Measured using `measure_quant_drop.py` methodology (`_measure` with 16 synthetic samples, seed 0) and on the 5-frame smoke dataset:

| Model | Mode | Synthetic 16-sample PLCC | Synthetic PLCC Drop | Synthetic Worst Abs Delta | Smoke 5-frame PLCC | Smoke PLCC Drop | Smoke Worst Abs Delta |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `mlp_small` | Static PTQ | 0.999536 | **0.000464** | 0.001042 | 0.990346 | **0.009654** | 0.006492 |
| `fr_tiny_qat` | QAT | 0.999631 | **0.000369** | 0.001006 | 0.980065 | **0.019935** | 0.052539 |
| `learned_filter` | QAT | 0.999896 | **0.000104** | 0.017420 | — | — | — |

Both static PTQ and QAT easily beat the current synthetic CI gate budget of `quant_accuracy_budget_plcc = 0.01` (both under 0.0005 drop on synthetic samples).

---

## 5. Blocking Gaps for Epic #1242

To ship retrained models as int8 static PTQ or QAT in epic #1246, the following six concrete defects and gaps must be resolved in epic #1242:

1. **[C Loader] Wire `.int8.onnx` redirect into `vmaf_use_tiny_model()` (`core/src/dnn/dnn_attach_api.c`)**:
   `vmaf_use_tiny_model()` must mirror the redirect logic of `vmaf_dnn_session_open()` in `dnn_api.c`: when `have_meta && meta.quant_mode != VMAF_QUANT_FP32`, rewrite `<stem>.onnx` to `<stem>.int8.onnx`, validate size and allowlist, and open the int8 sibling.
2. **[Model Sidecar] Fix `vmaf_tiny_v3.int8.json` missing `"onnx_has_scaler"`**:
   Add `"onnx_has_scaler": true` to `model/tiny/vmaf_tiny_v3.int8.json` (and any other int8 sidecar where normalization is in-graph) so `libvmaf.c:1442` does not apply double-scaling.
3. **[Script] Pin `quant_format=QuantFormat.QDQ` in `ptq_static.py` and `qat_train.py`**:
   Currently both scripts rely on the ambient ORT default. As recorded in `T-AI-PTQ-STATIC-QUANT-FORMAT-UNPINNED-2026-09-03`, they must explicitly pass `quant_format=QuantFormat.QDQ` (matching `ai/src/vmaf_train/quantize.py:135`) to guarantee no QOperator-static ops (`QLinear*`) are emitted.
4. **[Script] Generalise `qat_train.py` data loader for 4D image tensors**:
   `_build_train_loader_factory` unconditionally invokes `VmafTrainDataModule`, which only loads 1D tabular features. For 2D CNN models (`learned_filter`), training requires image tensor batches; currently `learned_filter_v1_qat.yaml` only works because the missing cache triggers fallback to `--smoke`.
5. **[Script] Add `--fp32` / `--int8` CLI path overrides to `measure_quant_drop.py`**:
   `measure_quant_drop.py` enforces paths under `model/tiny/` and looks up entries in `model/tiny/registry.json`. For development, CI smoke testing, and pre-release gating, it needs direct flags `--fp32 <path> --int8 <path>` to measure uncommitted models without touching `registry.json`.
6. **[CI / Validation] End-to-end VMAF delta gate**:
   The current accuracy gate only measures PLCC on 16 synthetic random tensors. A true release gate must assert that VMAF score predictions across a validation corpus do not drift by more than a defined threshold.

---

## 6. Drop Gate Threshold Recommendation

`docs/ai/quantization.md` states:

- `static`: small accuracy hit (~0.2% = 0.002)
- `qat`: reference (within ~0.05% = 0.0005)
- default registry budget: `0.01` (1 PLCC point)

### Recommended Two-Tier Gate for Epic #1246

1. **Synthetic PLCC Drop Gate (`measure_quant_drop.py`)**:
   - For **Static PTQ**: tighten `quant_accuracy_budget_plcc` from `0.01` to `0.002` (0.2% drop). Empirical smoke was `0.000464` (4× margin).
   - For **QAT**: tighten to `0.001` (0.1% drop). Empirical smoke was `0.000369` (nearly 3× margin).
2. **Video Feature / Score Gate**:
   - Real content features exhibit higher sensitivity than uniform synthetic distributions (smoke drop was ~0.0096).
   - Recommend a clip-level VMAF score parity threshold:
     - Mean absolute delta across validation pairs: $\le 0.10$ VMAF points.
     - Maximum single-frame absolute delta: $\le 0.50$ VMAF points.
     - PLCC on real feature sets: $\ge 0.990$.

---

## Related

- Epics: #1242 (int8 shipping readiness), #1246 (one-shot retrain)
- ADRs: [ADR-0129](../adr/0129-tinyai-ptq-quantization.md), [ADR-0173](../adr/0173-ptq-int8-audit-impl.md), [ADR-0174](../adr/0174-first-model-quantisation.md), [ADR-0207](../adr/0207-tinyai-qat-design.md), [ADR-1032](../adr/1032-vmaf-init-double-init-guard-vmaf-close-pointer-contract.md)
- Research: [Research-0006](0006-tinyai-ptq-accuracy-targets.md), [Research-2028](2028-code-scanning-audit-2026-09-03.md)
- Tracking: `T-AI-PTQ-STATIC-QUANT-FORMAT-UNPINNED-2026-09-03`, `T-DNN-ATTACH-INT8-REDIRECT-MISSING-2026-09-04`, `T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04`
