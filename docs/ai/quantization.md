<!-- markdownlint-disable MD013 MD060 -->
# Tiny-AI int8 quantisation

Pick a quantisation mode, produce a `.int8.onnx` next to the fp32 model, and
check it against an accuracy budget. The fork supports three post-training
quantisation (PTQ) modes plus quantisation-aware training (QAT). Each model
records its quant decision in `model/tiny/registry.json`, with a PLCC budget the
CI harness enforces against the fp32 baseline.

Policy origin is [ADR-0129](../adr/0129-tinyai-ptq-quantization.md), audited and
scaffolded in [ADR-0173](../adr/0173-ptq-int8-audit-impl.md). The runtime
`.int8.onnx` redirect landed in
[ADR-0174](../adr/0174-first-model-quantisation.md). What the loader does today
is under [How the loader treats int8](#how-the-loader-treats-int8).

!!! note
    Every shipped int8 file is dynamic PTQ in QOperator format
    (`learned_filter_v1`, `nr_metric_v1`, `vmaf_tiny_v3`, `vmaf_tiny_v4`).
    Static PTQ and QAT are supported by the scripts and the loader, but no
    shipped registry row uses `quant_mode: "static"` or `"qat"`.

## Choose a mode

| Mode | Accuracy | Cost to produce | Best for |
| --- | --- | --- | --- |
| `fp32` | reference | none | new models, debug builds |
| `dynamic` | small hit (~0.5%) | one CLI call | models without a calibration set; deployment box differs from training box |
| `static` | small hit (~0.2%) | one calibration pass | models you own and can pin a calibration set for |
| `qat` | reference (within ~0.05%) | extra training phase, ~1.5x fp32 train time | models where static drops accuracy past the per-model budget |

Pick the cheapest mode that stays inside the `quant_accuracy_budget_plcc`
budget.

### Registry fields

| Field | Type | Default | Required when |
|---|---|---|---|
| `quant_mode` | `fp32` / `dynamic` / `static` / `qat` | `fp32` | always present (default `fp32`) |
| `quant_calibration_set` | path relative to the repo root | absent | `quant_mode == "static"` |
| `quant_accuracy_budget_plcc` | number in `[0, 1]` | `0.01` | always (the CI gate honours per-entry values) |

`fp32` keeps the loader on `<basename>.onnx`. The other three modes redirect the
loader to a sibling `<basename>.int8.onnx` produced by the scripts below. The
fp32 file stays on disk as the regression baseline. The full schema is in
[model-registry.md](model-registry.md).

## Produce int8 artefacts

### Dynamic PTQ

No calibration data is needed. The script wraps
`onnxruntime.quantization.quantize_dynamic`.

```bash
python ai/scripts/ptq_dynamic.py model/tiny/nr_metric_v1.onnx \
    --report-out runs/nr_metric_v1_dynamic_ptq.json
# -> model/tiny/nr_metric_v1.int8.onnx
```

`--report-out` writes a JSON report with the fp32 and int8 byte sizes, the
per-channel setting, the output path and `run_provenance`.

### Static PTQ

1. Build a calibration `.npz`: one entry per ONNX input name, each a stack of
   `[N, ...]` representative samples. No in-tree script writes it; hand-craft
   it from a parquet feature cache or decoded frames. For the feature-vector
   FR regressors, `vmaf-train quantize-int8` (see [training.md](training.md))
   runs static PTQ calibrated from a parquet feature cache directly, without
   an `.npz`.
2. Quantise:

    ```bash
    python ai/scripts/ptq_static.py model/tiny/nr_metric_v1.onnx \
        --calibration runs/nr_metric_v1_calibration.npz \
        --report-out runs/nr_metric_v1_static_ptq.json
    ```

3. The output goes to `<input>.int8.onnx`. Add the calibration path to the
   registry's `quant_calibration_set` field.

The optional report holds the calibration input names and sample count, the size
ratio and `run_provenance`. No calibration `.npz` is shipped: calibration sets
are not redistributable by default, so each operator builds their own.

### `vmaf-train quantize-int8`

A one-command static PTQ (QDQ format) from a parquet feature cache, with a drift
gate. It reports the int8-versus-fp32 RMSE on held-out samples and exits 2 when
the RMSE exceeds the gate.

```bash
vmaf-train quantize-int8 \
    --fp32 runs/fr_tiny_v1/fr_tiny_v1.onnx \
    --output runs/fr_tiny_v1/fr_tiny_v1.int8.onnx \
    --calibration ai/data/nflx_features.parquet \
    --json runs/fr_tiny_v1/quantize_int8.json
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--fp32` | required | Input fp32 `.onnx`. |
| `--output` | required | Output int8 `.onnx`. |
| `--calibration` | required | Parquet feature cache used for calibration (the schema `vmaf-train eval` consumes). |
| `--input-name` | `features` | ONNX input name. |
| `--n-calibration` | 512 | Calibration sample count. |
| `--batch-size` | 32 | Calibration batch size. |
| `--rmse-gate` | 1.0 | Exit 2 when the int8-versus-fp32 RMSE exceeds this. |
| `--json PATH` | unset | Write a JSON report with `run_provenance`. |

### Quantisation-aware training (QAT)

Use QAT when static PTQ exceeds the per-model `quant_accuracy_budget_plcc`, or
when the QAT-versus-static delta on real content justifies about 50 % extra
training time (Research-0006 section 4). On tiny models (about 10 K parameters
and fewer layers) QAT and static PTQ tend to agree inside the 0.002 budget, so
choose static PTQ for cost. On larger architectures with wider weight
distributions QAT typically wins. The measured delta is recorded per model in
its
ADR, for example [ADR-0208](../adr/0208-learned-filter-v1-qat-impl.md).

```bash
python ai/scripts/qat_train.py \
    --config ai/configs/learned_filter_v1_qat.yaml \
    --output model/tiny/learned_filter_v1.int8.onnx \
    --report-out runs/learned_filter_v1_qat.json
```

#### Phases

[ADR-0207](../adr/0207-tinyai-qat-design.md) defines four phases:

| Phase | What happens |
| --- | --- |
| 1. fp32 warm-start | Normal fp32 training. |
| 2. Fake-quant insertion | `torch.export` captures the trained module and `torchao.quantization.pt2e.prepare_qat_pt2e` inserts observers under `X86InductorQuantizer`'s default recipe: per-tensor `uint8` activations, per-channel symmetric `int8` weights on `ch_axis=0`. |
| 3. QAT fine-tune | Fine-tune at 10x reduced learning rate (default $\mathrm{fp32\_lr} / 10$). |
| 4. ONNX export | Copy the QAT-conditioned weights into a fresh fp32 module, export that graph, then run `onnxruntime.quantization.quantize_static` with a calibration set drawn from the QAT training distribution. |

The output is a QDQ-format `.int8.onnx`, structurally identical to the
static-PTQ artefact. The QAT effect is preserved entirely through weight
pre-conditioning.

#### CLI knobs

| Flag | Default | Meaning |
| --- | --- | --- |
| `--epochs-fp32` | 20 | Phase 1 epochs. |
| `--epochs-qat` | 10 | Phase 3 epochs. |
| `--lr-qat` | fp32 lr / 10 | Phase 3 learning rate. |
| `--n-calibration` | 64 | Calibration samples for phase 4. |
| `--smoke` | off | Skip both training phases (CI / dev round trip). |
| `--report-out` | unset | JSON with fp32/int8 outputs, parameter count, phase settings and `run_provenance`. |

The YAML config mirrors the `vmaf-train fit` shape plus a `qat:` block; a
complete example is
[`ai/configs/learned_filter_v1_qat.yaml`](../../ai/configs/learned_filter_v1_qat.yaml).
`ai.train.qat.run_qat(...)` exposes the same pipeline for direct Python use, as
tests do.

#### Training data

The config's `cache:` field points at the training corpus. The rank of
`qat.input_shape` decides which reader parses it, the same shape the trace uses,
so loader and trace cannot disagree:

| `qat.input_shape` rank | Loader | Cache format |
| --- | --- | --- |
| 2 (for example `[1, 6]`) | `vmaf_train.datamodule.VmafTrainDataModule` | `.parquet` (canonical-6 feature columns plus `mos`) or `.npz` (`features`, `scores`) |
| 4 (for example `[1, 1, 32, 32]`) | built-in NCHW image loader | `.npz` (below) |
| anything else | none; the run downgrades to `--smoke` with a message on stderr | none |

Rank 4 is also the default when `qat.input_shape` is absent, matching the
`[1, 1, 32, 32]` fallback the exporter traces with.

A rank-4 `.npz` carries the input batch under `x` (aliases `images`, `degraded`,
`input`) with shape `(N, C, H, W)`, and the target under `y` (aliases `targets`,
`clean`, `reference`, `output`) with the same leading dimension. Both are cast
to
float32. The archive is validated when the loader is built: a wrong extension,
an unknown array name, a non-4D input, a length mismatch or an empty cache all
exit before the fp32 warm-start burns an epoch. Batch size comes from the
config's `batch_size` (default 32).

```python
import numpy as np
np.savez(
    "ai/data/learned_filter_patches.npz",
    x=degraded_patches,   # (N, 1, 32, 32) float32
    y=clean_patches,      # (N, 1, 32, 32) float32
)
```

## How the loader treats int8

Both `vmaf_dnn_session_open` in
[`core/src/dnn/dnn_api.c`](../../core/src/dnn/dnn_api.c) and `vmaf_use_tiny_model`
in [`core/src/dnn/dnn_attach_api.c`](../../core/src/dnn/dnn_attach_api.c) use
the
same redirect logic to choose the file:

| Step | Check | On failure |
| --- | --- | --- |
| 1 | Validate the caller-supplied fp32 path: size cap and op allowlist. | Error. |
| 2 | Load the sidecar. If the caller path already ends in `.int8.onnx`, or `quant_mode: "fp32"`, the given file is the model. | Resolution stops here on success. |
| 3 | Strip a trailing `.onnx`, append `.int8.onnx`, run the same size and allowlist validation on that sibling. | A path overflowing the 4096-byte buffer returns `-ENAMETOOLONG`. |
| 4 | If the int8 file validates, load it instead of the fp32 file. | Go to step 5. |
| 5 | The int8 file is missing, over the size cap, or has a non-allowlisted op. | Log a `VMAF_LOG_LEVEL_DEBUG` line and load the fp32 baseline. The session still reports the sidecar's `quant_mode`; only the weights are fp32. |
| 6 | The int8 file passed step 3 but `vmaf_ort_open()` fails on it. | Retry the fp32 baseline once. A failure of that retry reaches `WARNING`. |

The redirect keys off `quant_mode != fp32` alone. It does not tell `dynamic`
from
`static` or `qat`, and neither registry nor sidecar records a wire format. The
allowlist scan in step 3 alone decides whether an int8 graph is acceptable.

Step 6 is real. An ONNX Runtime build without a kernel for a quantised op fails
session creation with `-EIO` and `Could not find an implementation for
ConvInteger(10)`. `model/tiny/nr_metric_v1.int8.onnx` (dynamic PTQ,
`ConvInteger`) hits this on such a build. The allowlist scan cannot see it,
because it checks op names, not whether the local runtime has a kernel. The
int8 attempt's error is logged at `DEBUG`, like step 5. Both loaders share step
6
through `vmaf_ort_open_with_fallback()` in
[`core/src/dnn/ort_backend.c`](../../core/src/dnn/ort_backend.c).

!!! warning
    A quantised model whose int8 file is absent or rejected still loads and
    still
    scores, at fp32 weights and fp32 speed. Nothing fails, and the scores are
    the
    fp32 baseline's rather than the int8 model's. The fallback is visible only
    at
    debug log level, and the `vmaf` CLI has no flag for it (the binary runs at
    `VMAF_LOG_LEVEL_INFO`). An API caller that sets
    `VmafConfiguration.log_level` to `VMAF_LOG_LEVEL_DEBUG` sees
    `int8 sidecar unavailable` (steps 3 to 5) or `int8 session open failed`
    (step 6) and can confirm which weights a session loaded.

### Integrity of the int8 file

The redirect does not verify `int8_sha256`. The fp32 sidecar records the digest
for its quantised sibling, but neither loader parses it. At load time the gates
are the 50 MB size cap and the op allowlist. Integrity is enforced elsewhere:

- `ai/scripts/validate_model_registry.py` and `core/test/dnn/test_registry.sh`
  compare `int8_sha256` against the file on disk in CI;
- `--tiny-model-verify` checks the Sigstore bundle
  ([ADR-0211](../adr/0211-model-registry-sigstore.md),
  [model-registry.md](model-registry.md)).

A digest check inside the loader would change the ADR-1032 fallback semantics (a
mismatch would need its own outcome, distinct from "int8 absent") and needs its
own ADR. It is not in the tree.

### The `onnx_has_scaler` contract

Feature-vector tiny models (`vmaf_tiny_v2` to `v4`, `fr_regressor_v*`) ship the
StandardScaler in one of two ways, and the sidecar is the only thing that says
which:

| Where the scaler lives | Sidecar | Runtime |
| --- | --- | --- |
| In the graph: `Sub` (mean) and `Div` (std) Constant nodes before the first `Gemm` | declares `"onnx_has_scaler": true` | feeds raw canonical-6 values |
| In the runtime | carries `input_mean` / `input_std` (or `feature_mean` / `feature_std`), omits `onnx_has_scaler` | `core/src/libvmaf.c` normalises the vector before inference |

The two must not both apply. If a scaler-baking graph ships without the
declaration, the runtime double-scales and the score is meaningless. Measured on
the Netflix `src01_hrc00/hrc01_576x324` pair with `vmaf_tiny_v3.int8.onnx`:

| Sidecar | Pooled `vmaf_tiny_model` | Per-frame PLCC vs fp32 |
| --- | --- | --- |
| without the declaration | 16.020865 | 0.975443 |
| with the declaration | 71.952113 | 0.999876 |
| fp32 baseline | 72.359458 | 1 |

Quantising a model does not change which case applies: `ptq_dynamic.py`,
`ptq_static.py` and `qat_train.py` keep the scaler nodes in the graph. An
`.int8.onnx` therefore needs the same declaration its fp32 parent has. Three
gates enforce it over every `model/tiny/*.int8.onnx`:

```bash
bash core/test/dnn/test_registry.sh                                   # meson `dnn` suite
python -m pytest python/test/model_registry_schema_test.py -q         # python suite
python ai/scripts/validate_model_registry.py                          # CI registry gate
```

Each loads the graph with `onnx` when installed (falling back to a protobuf byte
scan) and fails when a graph with both `Sub` and `Div` has a companion sidecar
that does not declare `"onnx_has_scaler": true`. The companion sidecar of
`foo.int8.onnx` is `foo.int8.json` when present, otherwise the fp32 sidecar
`foo.json`, the same order `vmaf_dnn_sidecar_load` uses.

## Wire format: QOperator versus QDQ

ONNX encodes an int8 graph in one of two wire formats, and the fork does not
treat them interchangeably. `quant_mode` records how a model was quantised, not
the format. The format follows from the producer script:

| Producer | `quant_mode` | Wire format | Int8 ops emitted |
| --- | --- | --- | --- |
| `ai/scripts/ptq_dynamic.py` | `dynamic` | QOperator | `DynamicQuantizeLinear`, `MatMulInteger`, `ConvInteger` |
| `ai/scripts/ptq_static.py` | `static` | QDQ (pinned) | `QuantizeLinear` / `DequantizeLinear` wrapping stock `Conv` / `Gemm` / `MatMul` |
| `ai/src/vmaf_train/quantize.py` (`vmaf-train quantize-int8`) | `static` | QDQ (pinned explicitly) | as above, restricted to `Gemm` / `MatMul` / `Conv` |
| `ai/scripts/qat_train.py` | `qat` | QDQ | as above; the export phase runs `quantize_static` |

### What the fork ships

QOperator, for every model shipped today. All four `.int8.onnx` files under
`model/tiny/` come from `ptq_dynamic.py`, and ONNX Runtime's `quantize_dynamic`
takes no `quant_format` argument, so dynamic PTQ is QOperator-only. No shipped
model contains a `QuantizeLinear` or `DequantizeLinear` node: QDQ is a supported
input, not a shipped output.

??? note "Node census of the shipped int8 files"
    ```text
    learned_filter_v1.int8.onnx   10 ConvInteger   10 DynamicQuantizeLinear
    nr_metric_v1.int8.onnx        11 ConvInteger   12 DynamicQuantizeLinear   1 MatMulInteger
    vmaf_tiny_v3.int8.onnx         3 MatMulInteger  3 DynamicQuantizeLinear
    vmaf_tiny_v4.int8.onnx         4 MatMulInteger  4 DynamicQuantizeLinear
    ```

### What the loader accepts

QOperator dynamic and QDQ load. QOperator static is rejected.

The gate is [`core/src/dnn/op_allowlist.c`](../../core/src/dnn/op_allowlist.c).
`vmaf_dnn_scan_onnx` walks every ONNX file node by node, recursively into `Loop`
and `If` subgraphs. A node whose `op_type` is not on the list makes
`vmaf_dnn_validate_onnx` return `-EPERM`. The five quantisation entries are:

| Op | Format | Role |
| --- | --- | --- |
| `QuantizeLinear` | QDQ | fp32 to int8 with a calibrated scale and zero-point |
| `DequantizeLinear` | QDQ | int8 back to fp32 on leaving a quantised region |
| `DynamicQuantizeLinear` | QOperator dynamic | per-tensor scale and zero-point computed at run time |
| `MatMulInteger` | QOperator dynamic | integer matrix multiply |
| `ConvInteger` | QOperator dynamic | integer convolution |

QDQ loads because it leaves the arithmetic on stock `Conv`, `Gemm` and `MatMul`
nodes, which the allowlist already carries for fp32 models. The QDQ pair adds
only
the two scale-carrying ops.

QOperator static does not load. It folds the arithmetic into fused `QLinear*`
ops (`QLinearConv`, `QLinearMatMul`, `QGemm`, `QLinearAdd` and others) and the
allowlist has none of them. This is why
[`ai/src/vmaf_train/quantize.py`](../../ai/src/vmaf_train/quantize.py) pins
`quant_format=QuantFormat.QDQ` instead of relying on a default. Admitting a
`QLinear*` op would widen the model attack surface and is an allowlist change
that needs a security review, not a documentation change.

## Accuracy gates

Two gates guard int8 quality. The first runs in CI, the second is the strict
clip-level check for release quality.

| Gate | Script | Input | Threshold | Exit codes |
| --- | --- | --- | --- | --- |
| `ai-quant-accuracy` (CI) | `ai/scripts/measure_quant_drop.py` | 16 deterministic synthetic samples (seed 0) | PLCC drop at most the per-model `quant_accuracy_budget_plcc` | 0 pass, 1 over budget or missing file, 2 bad invocation |
| Clip-level parity | `ai/scripts/validate_quant_parity.py` | real feature clips (default `testdata/scores_cpu_576.json`; `.parquet` and `.npz` supported) | mean absolute delta at most 0.10 VMAF (`--max-mean-delta`), maximum single-frame delta at most 0.50 (`--max-single-delta`), PLCC at least 0.990 (`--min-plcc`) | 0 all pass, 1 any threshold breached, 2 invocation or file errors |

### CI gate: `ai-quant-accuracy`

The job runs in the `Tiny AI` job of
[`tests-and-quality-gates.yml`](../../.github/workflows/tests-and-quality-gates.yml)
(wired by [ADR-0174](../adr/0174-first-model-quantisation.md)). It calls
`measure_quant_drop.py --all`, which walks the registry, runs each non-`fp32`
model through fp32 and int8 ORT sessions on 16 deterministic synthetic samples,
and asserts that the aggregate Pearson correlation drop stays below the
per-model budget. A budget violation fails the PR. Run it locally:

```bash
python ai/scripts/measure_quant_drop.py --all \
    --out-json runs/quant_drop_gate.json
```

`--out-json` keeps the per-model gate rows and the `run_provenance` block. Use
it
as model-card evidence, or to compare a refreshed int8 sidecar with a previous
CI
gate.

#### Gate a model that is not in the registry

`--all` and the positional form resolve through
[`model/tiny/registry.json`](../../model/tiny/registry.json): the model must live
under `model/tiny/` and the budget comes from its entry. A model that is not
committed yet (PTQ or QAT scratch output, a CI artefact, a release candidate)
has
neither. The `--fp32` and `--int8` overrides measure an explicit pair and touch
no
registry:

```bash
python ai/scripts/measure_quant_drop.py \
    --fp32 /tmp/train_out/mlp_small_final.onnx \
    --int8 /tmp/train_out/mlp_small_final.ptq_static.int8.onnx \
    --budget 0.002 \
    --id mlp_small_static_ptq \
    --out-json /tmp/quant_drop.json
```

```text
[PASS] mlp_small_static_ptq     mode=override PLCC=0.999536  drop=0.000464  budget=0.0020  worst_abs=0.0010
```

| Flag | Meaning |
| --- | --- |
| `--fp32 PATH` | fp32 ONNX to measure. Required together with `--int8`; any path. |
| `--int8 PATH` | int8 ONNX to measure against it. |
| `--budget FLOAT` | PLCC-drop budget for the pair (default `0.01`, the registry-wide default). Research-2029 section 6 recommends `0.002` for static PTQ and `0.001` for QAT. |
| `--id NAME` | Label in the console line and the report. Defaults to the fp32 filename without `.onnx`. |

The overrides are mutually exclusive with `--all` and with the positional
argument: passing both exits 2. The report keeps the registry-run shape, with
`"quant_mode": "override"` on the single row.

### Clip-level gate: `validate_quant_parity.py`

Synthetic random tensors test operator fidelity but are less sensitive than real
natural-video features. `validate_quant_parity.py` evaluates models on real
feature clips against the strict Research-2029 section 6 thresholds in the table
above.

```bash
# Gate all shipped tabular FR models against the default thresholds:
python ai/scripts/validate_quant_parity.py --all \
    --out-json runs/quant_parity_gate.json

# Gate a specific model or a direct fp32/int8 pair:
python ai/scripts/validate_quant_parity.py --model vmaf_tiny_v3
python ai/scripts/validate_quant_parity.py \
    --fp32 /tmp/model.onnx \
    --int8 /tmp/model.int8.onnx \
    --features ai/testdata/bisect/features.parquet \
    --max-mean-delta 0.10 \
    --max-single-delta 0.50 \
    --min-plcc 0.990
```

!!! note
    The shipped `vmaf_tiny_v3` and `vmaf_tiny_v4` are un-retrained dynamic PTQ
    models. They reach high linear correlation ($\text{PLCC} \ge 0.994$), but
    dynamic activation quantisation adds a modest absolute score shift
    ($\text{mean } |\Delta| \approx 0.31\text{--}0.56$,
    $\text{max } |\Delta| \approx 0.54\text{--}0.97$). Running
    `validate_quant_parity.py` with the default thresholds therefore fails
    closed, as designed. Reaching mean delta $\le 0.10$ and max delta $\le 0.50$
    needs full QAT retraining against the `vmaf_v1.0.16_3d0h` teacher on the
    152k-clip training corpus (Epic #1246, RC9).

## Currently quantised models

| Model id | Mode | Size shrink | Measured drop | Budget |
| --- | --- | --- | --- | --- |
| `learned_filter_v1` | dynamic | 2.4x (80 KB to 33 KB) | 0.000117 (PLCC 0.999883) | 0.01 |
| `nr_metric_v1` | dynamic | 2.0x (119 KB to 58 KB) | 0.007674 (PLCC 0.992326) | 0.01 |
| `vmaf_tiny_v3` | dynamic | 0.95x (4 496 B to 4 267 B) | 0.000120 (PLCC 0.999880) | 0.01 |
| `vmaf_tiny_v4` | dynamic | 1.8x (14 046 B to 7 769 B) | 0.000145 (PLCC 0.999855) | 0.01 |

`vmaf_tiny_v3` and `vmaf_tiny_v4` joined the dynamic-PTQ family in
[ADR-0275](../adr/0275-vmaf-tiny-v3-v4-ptq.md). Their model cards carry the
reproduction commands and measured PLCC drops:
[`vmaf_tiny_v3`](models/vmaf_tiny_v3.md#quantisation-dynamic-ptq-int8-sidecar-adr-0275)
and
[`vmaf_tiny_v4`](models/vmaf_tiny_v4.md#quantisation-dynamic-ptq-int8-sidecar-adr-0275).

## Propose a model for quantisation

1. Run `ai/scripts/ptq_<mode>.py` to produce the int8 file.
2. Compute fp32 versus int8 PLCC on the soak fixture.
3. In the PR description, paste the PLCC numbers and the fp32 / int8 inference
   time ratio on at least one CPU.
4. Update `model/tiny/registry.json`: flip `quant_mode` to the chosen mode, set
   `quant_accuracy_budget_plcc` (default 0.01, one PLCC point), and add
   `quant_calibration_set` if `static`.
5. Land the int8 ONNX next to the fp32 file.

The reviewer compares the measured drop against the budget. If a static run
misses the budget, escalate to QAT in a follow-up PR; do not relax the budget.

## Caveats

- **`ptq_static.py` pins `quant_format=QuantFormat.QDQ`.** Like
  `ai/src/vmaf_train/quantize.py`, it pins QDQ so emitted static graphs contain
  only allowlisted ops. A QOperator-static graph (`QLinearConv`,
  `QLinearMatMul`, `QGemm`, ...) would be rejected by
  `core/src/dnn/op_allowlist.c` and libvmaf would silently fall back to fp32.
  `test_ptq_static_full_roundtrip` in `ai/tests/test_ptq_scripts.py` tests this
  contract.
- **Calibration sets are not redistributable** by default. Operators build their
  own from a parquet feature cache.
- **int8 can be slower than fp32.** VNNI / DLBoost speedup applies only to Intel
  CPUs from Cascade Lake on; ARMv8.2 and later have int8 dot-product. Without
  either, the int8 path can run slower than fp32. The overhead is the
  QOperator-dynamic requantise chain, not QDQ (no shipped model contains a
  `QuantizeLinear` node). Every `MatMulInteger` / `ConvInteger` is preceded by a
  `DynamicQuantizeLinear` that recomputes scale and zero-point on every
  inference, and followed by a `Cast` / `Mul` / `Add` chain converting the int32
  accumulator to fp32. Without an integer dot-product instruction these are pure
  overhead. The loader is bit-depth-agnostic and still picks the int8 model when
  the registry says so; measuring runtime performance is the operator's job.

## History

- **2026-09-05.** The `onnx_has_scaler` double-scaling defect was fixed in
  `model/tiny/vmaf_tiny_v3.int8.json`, which had shipped without the declaration
  (`T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04`). The three gates
  above were added to prevent a repeat.
- **ADR-1293.** QAT moved off
  `torch.ao.quantization.quantize_fx.prepare_qat_fx`,
  which PyTorch deprecated wholesale (the repository treats its
  `DeprecationWarning` as an error), to `torchao` PT2E
  ([ADR-1293](../adr/1293-tinyai-qat-torchao-pt2e.md)). Weight quantisation is
  unchanged. The activation range widened from the old reduce-range [0, 127] to
  the full [0, 255], which ORT `quantize_static` has always used on the other
  side of the handoff. Phase 4 also moved to the `torch.export`-based ONNX
  exporter: the export target is a plain fp32 module, so the quantisation
  buffers that forced the legacy TorchScript path are gone.
- **ADR-1032, fp32 fallback.**
  [ADR-0174](../adr/0174-first-model-quantisation.md)
  section 2 originally specified that a missing int8 file returns a negative
  error ("no silent fp32 fallback, that would mask deployment
  misconfigurations").
  [ADR-1032](../adr/1032-vmaf-init-double-init-guard-vmaf-close-pointer-contract.md)
  Fix 3 replaced the hard error with the fallback on a "better degraded than
  dead" rationale, and the code has matched ADR-1032 since. ADR-0174 is Accepted
  and therefore frozen, so it still reads the old way. This page is
  authoritative
  for the runtime behaviour.
- **QAT data dispatch.** Before the rank-based loader dispatch, every config
  went
  to `VmafTrainDataModule`, which only materialises rank-2 tabular rows. 2D CNN
  configs such as `learned_filter_v1_qat.yaml` could not train for real. They
  only appeared to work because their parquet cache is uncommitted, so the
  missing-cache branch silently downgraded the run to smoke mode (Research-2029
  section 5 gap 4).
- **`nr_metric_v1` export (T5-3d).** The original ONNX export tripped ORT shape
  inference in `quantize_dynamic` with `Inferred shape and existing shape differ
  in dimension 0: (128) vs (1)`. `torch.onnx.export` had emitted every
  initialiser into `graph.value_info` with static-shape annotations that did not
  survive the dynamic batch axis. The exporter
  (`ai/src/vmaf_train/models/exports.py`) and `ai/scripts/ptq_dynamic.py` now
  strip those duplicates, the same workaround introduced for
  `vmaf_tiny_v1*.onnx`
  in PR #174 (T5-3e).
