# Tiny-AI — PTQ accuracy across Execution Providers

Empirical comparison of int8 post-training-quantisation (PTQ) PLCC
drop across the Execution Providers (EPs) the fork actually has
hardware for: ORT `CPUExecutionProvider`, ORT
`CUDAExecutionProvider`, and the Intel OpenVINO runtime targeting
either an Intel Arc dGPU or the OpenVINO CPU plugin.

The harness lives in
[`ai/scripts/measure_quant_drop_per_ep.py`](../../ai/scripts/measure_quant_drop_per_ep.py)
and is the sibling of the CPU-only
[`measure_quant_drop.py`](../../ai/scripts/measure_quant_drop.py)
gate. The CPU gate is what CI enforces; this script is the
investigation tool used to verify that a model that passes on CPU
also behaves on GPU EPs before we ship it as a non-CPU default.

## What it measures

For each `(model, EP)` pair:

1. Load the fp32 ONNX through the chosen EP. The script reuses the
   `_load_session` workaround introduced in PR #165 — some shipped
   ONNX (`vmaf_tiny_v1.onnx`,
   `vmaf_tiny_v1_medium.onnx`) carry a stale
   `external_data.location` pointing at a renamed
   `mlp_*_final.onnx.data`; the loader rewrites the entry in
   memory.
2. Either reuse the registry-shipped int8 sibling
   (`<basename>.int8.onnx`) or, for fp32-only baselines passed via
   `--extra-fp32`, materialise a dynamic-PTQ int8 graph at runtime
   (matches the published mode for `learned_filter_v1`).
3. Run 16 deterministic synthetic samples (RNG seed 0, fp32 input
   in `[0, 1)`) through both fp32 and int8, collect the headline
   output, compute the Pearson linear correlation across the
   concatenated outputs, report `1 - PLCC` as the *drop* and assert
   it stays under the per-model
   `quant_accuracy_budget_plcc` from
   [`model/tiny/registry.json`](../../model/tiny/registry.json).
4. The script also tracks the worst per-sample max-absolute delta
   so that a degenerate model whose PLCC is undefined (constant
   output) still surfaces actual numerical divergence.

When invoked with `--gate`, the exit code is non-zero if any pair
exceeds its budget — useful for ad-hoc CI. The default mode is
"investigate" (always exit 0; emit JSON + Markdown).

## Running it

```bash
# CUDA EP needs the bundled CUDA-12 ABI .so files on LD_LIBRARY_PATH;
# our system has CUDA 13 installed but ORT-GPU 1.25 wheels expect 12.
SP="$VIRTUAL_ENV/lib/python3.14/site-packages/nvidia"
export LD_LIBRARY_PATH="$SP/cublas/lib:$SP/cudnn/lib:$SP/cuda_nvrtc/lib:$SP/cuda_runtime/lib:$SP/cufft/lib:$SP/curand/lib:$SP/cusolver/lib:$SP/cusparse/lib:$SP/cuda_cupti/lib:$SP/nvtx/lib:$SP/nvjitlink/lib"

python ai/scripts/measure_quant_drop_per_ep.py \
    --eps cpu cuda openvino \
    --extra-fp32 vmaf_tiny_v1.onnx vmaf_tiny_v1_medium.onnx \
    --openvino-device GPU.0 \
    --out runs/quant-eps-$(date +%Y-%m-%d)
```

Outputs `results.json` (machine-readable) and `results.md` (the
human-readable table that goes into the research digest). The JSON
report carries the shared `run_provenance` block, recording the script
revision, argv, selected EPs, hardware tag, tiny-model registry input,
optional fp32 baselines, and both report targets. The `runs/` directory
is gitignored — copy the headline numbers into
[`docs/research/0006-tinyai-ptq-accuracy-targets.md`](../research/0006-tinyai-ptq-accuracy-targets.md)
when refreshing the GPU-EP findings section.

## Headline findings (2026-04-28)

CPU EP and CUDA EP agree to 6 decimal places on every shipped tiny
model; the OpenVINO CPU plugin agrees to ~10⁻⁴ PLCC drop. The
Intel Arc A380 (`GPU.0` through OpenVINO 2026.1) is currently
**int8-broken** — `Conv`-based int8 graphs fail to compile, MLP
int8 graphs compile but emit `inf`/`NaN`. The fp32 path is healthy
on Arc, so the runtime can fall back to fp32 (or the OpenVINO CPU
plugin) for a reliable inference there.

See the full table + failure-mode breakdown in
[`docs/research/0006-tinyai-ptq-accuracy-targets.md`](../research/0006-tinyai-ptq-accuracy-targets.md)
§"GPU-EP quantisation".

## When to re-run

- After bumping `onnxruntime-gpu` or `openvino` major versions
  (the int8 layout failure on Arc may resolve in a later
  intel_gpu plugin).
- Before adding a new int8 model to the registry — paste the
  CUDA-EP row into the model-card and the research digest.
- When changing the dynamic-PTQ defaults (e.g. switching from
  per-tensor to per-channel weight quantisation).

## Limitations

- The 16 synthetic samples are deterministic but not
  distribution-realistic. The CPU gate's number is the one that
  matches the soak-test fixtures; this harness's number is a
  cross-EP comparison, not a regression on real video.
- The OpenVINO path uses the native `openvino` Python runtime
  (not `onnxruntime-openvino`) because no `cp314` wheel for the
  ORT-OpenVINO bridge exists on PyPI as of this writing. The
  numerics are equivalent; the API is just different.
- Only the first model output is compared. Multi-output models
  would need a small extension to the runner.
