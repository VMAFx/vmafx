<!-- markdownlint-disable MD013 MD060 -->
# Research-0006: Tiny-AI PTQ — accuracy regression targets, ORT API comparison, calibration-set sourcing

- **Status**: Active
- **Workstream**: [ADR-0129](../adr/0129-tinyai-ptq-quantization.md)
- **Last updated**: 2026-04-28 (T5-3e: empirical GPU-EP
  measurements landed)

## Question

For each of the three int8 quantisation modes (static PTQ, dynamic PTQ,
QAT) applied to fork-trained tiny-AI models, what are the concrete
trade-offs? Specifically:

1. What accuracy drop should we budget per mode, expressed as a Pearson
   linear correlation (PLCC) delta against the fp32 baseline on the
   soak-test fixtures?
2. Which ONNX Runtime API version / API surface do we target?
3. Where does the calibration dataset for static PTQ come from, and
   how do we keep it reproducible?
4. What does the QAT re-training cost look like for our existing
   tiny-AI models?

## Sources

- [ONNX Runtime quantization guide](https://onnxruntime.ai/docs/performance/model-optimizations/quantization.html)
  — authoritative for the static / dynamic API and the QDQ operator
  layout.
- [PyTorch Quantization (`torch.ao.quantization`)](https://pytorch.org/docs/stable/quantization.html)
  — the QAT path; `prepare_qat_fx` is the modern API.
- Microsoft Research, "Integer Quantization for Deep Learning Inference"
  (Krishnamoorthi, 2018) — the canonical survey; numerical bounds for
  PTQ accuracy drop are tabulated by architecture family.
- NVIDIA "Achieving FP32 Accuracy for INT8 Inference Using Quantization
  Aware Training with TensorRT" — QAT best practices; the "recover 95%
  of fp32 accuracy" target comes from this paper.
- The existing fork trainer under [`ai/`](../../ai/) — provides the
  PyTorch Lightning modules that `qat_train.py` will extend.
- `model/registry.json` — current schema and per-model metadata.
- Soak-test fixtures under [`testdata/`](../../testdata/) — the same
  YUV clips used to validate fp32 tiny-AI models are the reference
  for the PLCC gate.

## Findings

### 1. Accuracy budgets per mode

Empirical starting points — to be tightened per-model once we measure
real numbers:

| Mode | Typical PLCC drop vs fp32 | Starting budget |
|---|---|---|
| **Dynamic PTQ** | 0.003 – 0.015 | 0.01 |
| **Static PTQ** | 0.001 – 0.008 | 0.005 |
| **QAT** | 0.0002 – 0.003 | 0.002 |

The wide bands come from architecture dependence. Tiny convolutional
nets (the shape our tiny-AI models take) sit in the lower half of each
band because they have few layers where quantisation error can
accumulate. Transformer-ish models would sit at the high end, but we
don't train any of those in-tree.

The budget is the **hard gate**: the `ai-quant-accuracy` CI leg
refuses to merge a quantised model PR if the measured PLCC drop
exceeds `quant_accuracy_budget_plcc` in the registry. A model for
which dynamic-budget is exceeded is a candidate for static; static
over budget → QAT.

### 2. ONNX Runtime API surface

Three relevant APIs, all from `onnxruntime.quantization`:

- `quantize_dynamic(model_input, model_output, weight_type=QuantType.QInt8)`
  — one-liner. Quantises weights offline; activations are scaled at
  runtime from observed statistics. Ideal for our `ptq_dynamic.py`
  script.
- `quantize_static(model_input, model_output, calibration_data_reader, quant_format=QuantFormat.QDQ)`
  — requires a `CalibrationDataReader` subclass that yields
  representative inputs. QDQ format (Quantize-Dequantize pair) is the
  portable SPIR-V-like format that runs on any EP; QOperator format
  is faster but locked to CPU EP.
  We pick **QDQ** for portability — it works when we plumb through
  CUDA / DirectML / CoreML EPs later.
- `torch.ao.quantization.prepare_qat_fx(model, qconfig_mapping, example_inputs)`
  then standard PyTorch training loop, followed by `convert_fx` →
  `torch.onnx.export(..., opset_version=17)`. The opset matters —
  opset 17 is the first where QDQ ops round-trip cleanly from PyTorch
  to ORT.

All three APIs are stable as of ONNX Runtime 1.16 (Q4 2023), which is
well below our current minimum (`onnxruntime>=1.19`). No version bumps
needed.

### 3. Calibration dataset sourcing

Static PTQ needs **~100–1000** representative input frames that cover
the distribution the model sees at inference time. Two candidate
sources for the fork's tiny-AI models:

- **Option A — a curated subset of `testdata/`**: pick 10 frames each
  from 20 diverse clips (SD / HD / 4K, film / animation / sports /
  low-bitrate AV1 / HDR-tone-mapped). Total ~200 frames. Trivial to
  version-control; reproducible.
- **Option B — a synthetic calibration set**: generate frames via a
  procedural pattern generator that hits every quantisation bin.
  Smaller on disk but less representative of the real input
  distribution.

Recommendation: **Option A**, stored under `ai/calibration/<model-
name>/`, pointed to by the `quant_calibration_set` field in
`model/registry.json`. Tracked via git LFS (already set up for
fork-trained `.onnx` files).

Reproducibility: the calibration-set generation script
(`ai/scripts/gen_calibration.py`) takes a set of clip paths + frame
indices from a YAML spec, decodes them, and dumps them as raw
tensor-shaped binary. The YAML spec is checked in; the binary is in
git LFS. Anyone can reproduce the calibration set from source clips.

### 4. QAT re-training cost

For the current largest shipped tiny-AI model
(`vmaf_tiny_v1_medium.onnx` — the `mlp_medium` variant landed by
[PR #158](https://github.com/VMAFx/vmafx/pull/158); originally
referenced in early drafts of this digest as a `tiny-vmaf-v2`
prototype before the LOSO 3-arch evaluation
([Research-0023](0023-loso-3arch-results.md)) settled on
`mlp_medium` as the production architecture and the model was
renamed under the `vmaf_tiny_v1` family):

- fp32 training: ~45 min on one RTX 4070 for 30 epochs.
- QAT phase: 10 epochs at ~1.5× per-epoch cost ≈ 25 min additional.

So QAT adds ~50% to total training time. On the smaller fork-trained
tiny models (`vmaf_tiny_v1.onnx` — `mlp_small`, ~4 MB), the QAT
addition is under 10 minutes — small enough that QAT becomes the
default once a model hits an accuracy-budget failure.

The `qat_train.py` script wraps the existing Lightning module. The
developer invocation is:

```bash
python ai/scripts/qat_train.py \
    --model-config ai/configs/tiny-vmaf-v1.yaml \
    --fp32-checkpoint ai/checkpoints/tiny-vmaf-v1.ckpt \
    --output-onnx model/tiny-vmaf-v1.int8.onnx
```

All paths live in the registry schema, so the CI gate can reproduce it.

### 5. Runtime-side considerations

The ORT `CPUExecutionProvider` picks up int8 QDQ ops automatically on
VNNI-capable CPUs (Skylake-X and newer on x86; Neoverse N1 and newer
on ARM). Older CPUs fall back to fp32 emulation of the int8 ops, which
is **slower** than pure fp32 would have been — so on a pre-VNNI Xeon,
shipping a quantised model would be a net loss.

Mitigation: the registry gains a `quant_min_cpu_features` array (e.g.
`["vnni"]` or `["neoverse_n1"]`). The runtime in
`core/src/dnn/load.c` checks `cpuid` / `getauxval` at init, and
falls back to the fp32 model if the current CPU doesn't support the
required features. This is invisible to the user except in a log
message ("tiny-AI: CPU lacks VNNI, falling back to fp32 model").

### 6. Metric footprint — what goes in the runtime log

For each quantised model load, we log once at startup:

```text
[vmaf-dnn] Loaded model=tiny-vmaf-v1 quant=dynamic int8 fp32_fallback=false
```

and per-inference (at `VMAF_LOG_DEBUG` only):

```text
[vmaf-dnn] tiny-vmaf-v1 int8 inference 0.42 ms (fp32 reference: 1.18 ms)
```

The reference fp32 time comes from a one-time calibration run at load
time, so the speedup is visible to the user without them having to
benchmark.

## Answered questions (for the ADR)

- **Which three scripts?** `ptq_static.py`, `ptq_dynamic.py`,
  `qat_train.py` under `ai/scripts/`.
- **Quant format?** QDQ (portable across EPs).
- **Registry schema?** Extend with `quant_mode`, `quant_calibration_set`,
  `quant_accuracy_budget_plcc`, `quant_min_cpu_features`.
- **Calibration data location?** `ai/calibration/<model-name>/`, LFS-
  tracked, generated by a deterministic script from a versioned YAML.
- **Accuracy gate?** New CI leg `ai-quant-accuracy` enforcing PLCC
  budget per model.
- **Runtime fallback on unsupported CPU?** Yes, fp32 fallback on VNNI-
  less CPUs.

## GPU-EP quantisation (T5-3e, 2026-04-28 — measured, no longer deferred)

Originally listed as an open follow-up "until a user surfaces a
non-CPU tiny-AI deployment". That framing was retired by the
[Section-A audit decisions](../../.workingdir2/decisions/section-a-decisions-2026-04-28.md)
§A.3.4 once the fork's bench host gained both an NVIDIA RTX 4090 and
an Intel Arc A380. Empirical run on 2026-04-28 with
`ai/scripts/measure_quant_drop_per_ep.py` (see
[`docs/ai/quant-eps.md`](../ai/quant-eps.md) for usage), 16 seeded
synthetic samples per (model, EP) pair, dynamic-PTQ (`QInt8` weights)
applied on the fly to the fp32-only baselines:

| Model | budget | CPU EP (ORT) | CUDA EP (ORT) | OpenVINO Arc A380 | OpenVINO CPU |
|---|---:|:---:|:---:|:---:|:---:|
| `learned_filter_v1` (Conv, shipped int8) | 0.0100 | 0.000117 PASS | 0.000117 PASS | compile-fail | 0.000133 PASS |
| `vmaf_tiny_v1` (mlp_small, dyn-PTQ) | 0.0100 | 0.000011 PASS | 0.000011 PASS | NaN/Inf | 0.000081 PASS |
| `vmaf_tiny_v1_medium` (mlp_medium, dyn-PTQ) | 0.0100 | 0.000006 PASS | 0.000006 PASS | NaN/Inf | 0.000052 PASS |

Arc A380 failure modes (intel_gpu plugin in OpenVINO 2026.1):

- `learned_filter_v1.int8.onnx` (ConvInteger + DynamicQuantizeLinear)
  fails to compile with `No layout format available for convolution:
  byxf / i32` from `add_required_reorders.cpp`.
- The MLP int8 graphs (MatMulInteger + DynamicQuantizeLinear) compile
  but emit `inf`/`NaN` for every input — int8 *correctness* is
  broken, not just performance.

Headline numbers reflect PLCC drop vs the per-model fp32 baseline.
Full per-EP detail (PLCC / worst |delta| / wall time) lives in the
local `runs/quant-eps-2026-04-28/results.{json,md}` (gitignored —
recreated by the harness). New runs also include `run_provenance` in
`results.json`, so copied research/model-card evidence can be traced to
the tiny-model registry, optional fp32 baselines, selected EPs, hardware
tag, argv, and report paths that produced it.

**Findings.**

1. **CPU EP and CUDA EP agree to 6 decimal places** on all three
   models. CUDA does not introduce a measurable additional PLCC drop
   on top of dynamic PTQ; on the MLPs the drop is at most
   ~1.1×10⁻⁵, well under the 1×10⁻² registry budget. The
   pre-shipped `learned_filter_v1.int8.onnx` survives migration to
   CUDA EP unchanged.
2. **OpenVINO CPU plugin** (Intel's CPU implementation of the same
   ONNX graph) agrees with ORT CPU to within ~10⁻⁴ PLCC drop. Slight
   divergence comes from OpenVINO's preferred graph rewrites — still
   inside budget for every model.
3. **OpenVINO GPU.0 plugin (Intel Arc A380) is currently
   int8-broken** for both ONNX-Runtime quantisation outputs we ship
   or generate. Two distinct failure modes:
   - The `Conv`-based `learned_filter_v1.int8.onnx` (using
     ConvInteger + DynamicQuantizeLinear) **fails to compile** —
     intel_gpu plugin reports `No layout format available for
     convolution: byxf / i32` from
     `add_required_reorders.cpp`.
   - The MLP int8 graphs (`MatMulInteger` + `DynamicQuantizeLinear`)
     **compile successfully but emit `inf`/`NaN`** for every input.
     This means int8 *correctness* is broken, not just performance.
4. **Arc fp32 path is healthy**: the same models run end-to-end on
   GPU.0 in fp32 and produce values within ~10⁻¹ of the OpenVINO CPU
   reference, so the issue is specifically the int8-quantisation
   lowering inside the intel_gpu plugin.

**Decision.** For now: do **not** rely on int8 quantisation when
targeting Intel Arc through OpenVINO. The runtime should fall back
to either (a) the OpenVINO CPU plugin or (b) the fp32 ONNX baseline
when `OpenVINOExecutionProvider` selects an `intel_gpu`-class
device. CUDA EP needs no special-casing — it runs the existing
ConvInteger / MatMulInteger graphs cleanly. This finding is the
basis for follow-up backlog row T5-3e-fix (Arc int8 support: revisit
when OpenVINO ships a newer intel_gpu plugin or when we explore
QDQ-format static PTQ, which sidesteps the DynamicQuantizeLinear
op).

**Reproduction.** Set up the `.venv` with `onnxruntime-gpu`,
`openvino`, and the bundled `nvidia-cublas-cu12` /
`nvidia-cudnn-cu12` / `nvidia-cufft-cu12` /
`nvidia-curand-cu12` / `nvidia-cusolver-cu12` /
`nvidia-cusparse-cu12` / `nvidia-cuda-runtime-cu12` /
`nvidia-cuda-cupti-cu12` / `nvidia-nvtx-cu12` /
`nvidia-nvjitlink-cu12` wheels (CUDA 12 ABI; ORT 1.25 expects this
even on a CUDA-13 host). Then:

```bash
SP=$VIRTUAL_ENV/lib/python*/site-packages/nvidia
export LD_LIBRARY_PATH="$SP/cublas/lib:$SP/cudnn/lib:$SP/cuda_nvrtc/lib:$SP/cuda_runtime/lib:$SP/cufft/lib:$SP/curand/lib:$SP/cusolver/lib:$SP/cusparse/lib:$SP/cuda_cupti/lib:$SP/nvtx/lib:$SP/nvjitlink/lib"
python ai/scripts/measure_quant_drop_per_ep.py \
    --eps cpu cuda openvino \
    --extra-fp32 vmaf_tiny_v1.onnx vmaf_tiny_v1_medium.onnx \
    --out runs/quant-eps-$(date +%Y-%m-%d)
```

## Open questions (for follow-up iterations)

- **Mixed-precision**: some models benefit from keeping the first and
  last layers at fp32 even under PTQ. The `quantize_static` API
  supports `nodes_to_exclude`; we'll expose this via an optional
  `quant_exclude_nodes` registry field if the first static
  conversion measurably improves when we exclude the input projection
  layer.
- **Calibration-set size floor**: 200 frames is a heuristic. We'll
  measure the PLCC-vs-size curve in the first static conversion PR
  to see if we can halve the calibration set without accuracy cost.
- **Per-channel vs per-tensor weight quantisation**: per-channel is
  the default and strictly better for convolutions; per-tensor is
  faster on older ARM. We default to per-channel.
- **Symmetric vs asymmetric activation quant**: ORT defaults to
  asymmetric; symmetric is slightly cheaper but has a larger dynamic-
  range penalty. Default to asymmetric unless a model flags
  otherwise in the registry.

## Next steps

1. Governance PR (this one) lands.
2. Audit PR: extend `model/registry.json` schema + JSON Schema
   validator + add the three scripts + wire the CI gate. No existing
   model changes `quant_mode` yet.
3. First conversion PR: `tiny-vmaf-v1` → dynamic. Accuracy
   measurement + soak-test result in PR body.
4. Per-model PRs follow as each model hits a speedup gap worth
   closing.
5. QAT is first exercised when a static conversion fails the budget
   — probably on a future larger tiny-AI model rather than the small
   ones already shipping.
