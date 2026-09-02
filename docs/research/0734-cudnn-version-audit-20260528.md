<!-- markdownlint-disable MD013 MD028 MD060 -->
# Research digest 0734 — cuDNN version audit for ONNX Runtime / tiny-AI inference
<!-- SPDX-License-Identifier: BSD-3-Clause-Plus-Patent -->
<!-- Copyright 2026 Lusoris -->

**Date:** 2026-05-28
**Triggered by:** PR #64 note — cuDNN ships on its own cadence, separate from the
CUDA Toolkit; the ORT cuDNN dependency must be tracked independently.
**Scope:** Identify which cuDNN version our ONNX Runtime build depends on; check
the latest cuDNN release for silent-corruption / performance fixes relevant to our
INT8/FP16 small-CNN inference workload.

---

## 1. Our cuDNN exposure — summary verdict: NONE (CPU-only ORT)

### 1.1 C library (`core/src/dnn/`)

`dev/Containerfile` installs ONNX Runtime from the **CPU-only** upstream tarball:

```text
ARG ORT_VERSION=1.26.0
RUN curl -fsSL \
    "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" \
    ...
```

The artifact `onnxruntime-linux-x64-<version>.tgz` (no `-gpu` suffix) is the
CPU-only build. It does **not** link cuDNN. `ort_backend.c` registers
`CUDAExecutionProvider` at runtime via `SessionOptionsAppendExecutionProvider_CUDA`
— that code path is present in the source but only reachable when the user
selects `VMAF_DNN_DEVICE_CUDA` **and** a GPU-capable ORT wheel is installed
at runtime. The default container and the C library build have no runtime
cuDNN dependency.

### 1.2 Python / training layer (`ai/`)

`ai/pyproject.toml` pins:

```toml
"onnxruntime>=1.26.0,<2.0",  # container pins 1.26.0 (ADR-0568); floor kept at 1.20 for host installs
```

This resolves to the `onnxruntime` PyPI package (CPU-only). The GPU variant
(`onnxruntime-gpu`) is **not** listed in any `pyproject.toml`, `requirements.txt`,
or `Containerfile` in tree.

The single cuDNN mention in the codebase is a documentation comment in
`ai/scripts/measure_quant_drop_per_ep.py`:

```text
# cuda  -> ORT CUDAExecutionProvider (NVIDIA, requires
#           CUDA 12 / cuDNN 9 runtime libs and an ORT-GPU wheel).
```

This is a usage note for an optional manual invocation; the script's installed
dependencies do not pull `onnxruntime-gpu`.

### 1.3 Containerfile CUDA install

Stage 2 of `dev/Containerfile` installs `cuda-toolkit` (the meta-package, currently
CUDA 13.2 — explicitly commented as "CUDA 13.2"). The meta-package does **not** pull
`libcudnn*`; cuDNN is a separate optional package on NVIDIA's apt repository.
No `apt-get install libcudnn*` line exists anywhere in the Containerfile.

---

## 2. Latest cuDNN version

**cuDNN 9.22.0** (May 2026). Confirmed via
`https://developer.nvidia.com/cudnn-downloads`.

Previous release: cuDNN 9.21.1.

ORT's CUDA EP compatibility table (ORT ≥ 1.18.x with CUDA 12.x requires
**cuDNN 9.x**). ORT 1.26.0 (CUDA 12 variant, if ever installed) would require
cuDNN ≥ 9.0.

---

## 3. cuDNN 9.22.0 release notes — verbatim excerpts

Source: `https://docs.nvidia.com/deeplearning/cudnn/backend/latest/release-notes.html`
(retrieved 2026-05-28).

### 3.1 Known issues in 9.22.0 (excerpted verbatim)

> "For certain convolution-related workloads, memory allocations are made that
> are not released until process termination."

> "Performance of some matrix multiplication operations through the cuBLASLt
> engine might be slower when run with CUDA Toolkit 13.2 Update 1 compared with
> earlier CUDA Toolkit 13.x versions."

> "Runtime compilation of `LayerNorm` and `RMSNorm` execution plans might be
> protracted on compute capability 12.0 devices."

> "Scaled dot-product attention bprop is not supported when both key/value have
> sequence length of 1."

### 3.2 Fixed in 9.21.1 (excerpted verbatim)

> "An issue where scaled dot-product attention causal forward could hang with
> extremely long sequence lengths on Blackwell-architecture GPUs has been fixed."

> "An issue where scaled dot-product attention forward could crash during
> execution plan creation when dropout was enabled on Blackwell-architecture
> GPUs has been fixed."

> "A performance regression when using the SM count attribute with Layer and RMS
> Normalization has been fixed."

### 3.3 Fixed in an earlier 9.x release (from web search aggregation)

> "Performance regression for convolutional workloads on Turing-architecture
> GPUs introduced in cuDNN 9.17.0 is now fixed."

> "Updated runtime fusion engine heuristics for INT8 for both Convolution and
> Matmul operations on the NVIDIA Hopper architecture to support new batched
> GEMM kernel, with improved fusion performance, and compilation time."

---

## 4. VMAF tiny-AI exposure classification

| cuDNN issue | Affected ops | Tiny-AI exposure | Assessment |
|---|---|---|---|
| Convolution memory not freed until process exit (9.22.0 KI) | Conv on CUDA | RELEVANT — we run small-CNN conv kernels via CUDA EP when GPU path is manually invoked | **Low risk**: not a correctness bug; memory is freed at process exit. Inference-server deployments that restart the process periodically are unaffected. Would become high-severity in a persistent daemon. |
| cuBLASLt matmul perf regression (CUDA 13.2 Update 1, 9.22.0 KI) | MatMul | MARGINALLY RELEVANT — DNN models include FC layers implemented as matmul | **Perf only, not correctness**. Affects CUDA 13.2 Update 1 specifically. Container pins CUDA 13.2 meta-package (pre-Update-1 per Containerfile comment); low exposure. |
| Turing conv perf regression (9.17.0–9.20.x) | Conv, Turing GPUs | RELEVANT if running on a Turing-class card (RTX 2000/3000-series) | **Fixed in 9.21.x**. If `onnxruntime-gpu` is ever installed against cuDNN 9.17–9.20 on Turing, affected. |
| INT8 Hopper fusion heuristic improvement | INT8 Conv + Matmul | RELEVANT — INT8 is one of our quantization modes | **Positive fix** (performance gain on Hopper). Not a bug. |
| Attention hang on Blackwell (9.21.1 fix) | Scaled-dot-product attention | NOT RELEVANT — tiny-AI models are CNN-only, no attention ops | Safe. |
| Attention dropout crash on Blackwell (9.21.1 fix) | Attention + dropout | NOT RELEVANT — no attention layers in tiny-AI models | Safe. |
| RNN ops | RNN/LSTM/GRU | NOT RELEVANT — we are CNN-only | Safe. |
| Large-batch specialised kernels | Large batch | NOT RELEVANT — single-frame-at-a-time inference | Safe. |

---

## 5. Recommendation

**Immediate action: NONE required.**

The fork's container and default Python environment install the CPU-only ORT 1.26.0
build; cuDNN is not a transitive dependency of any installed artifact. The `CUDA`
execution-provider code path in `ort_backend.c` is present but only reachable when a
user manually installs `onnxruntime-gpu` (not provided by the fork).

**Track separately:**

1. If a future PR adds `onnxruntime-gpu` to `ai/pyproject.toml` or the Containerfile,
   pin cuDNN ≥ 9.21.1 (fixes the Turing conv perf regression) and document the
   constraint in `dev/Containerfile` and `ai/pyproject.toml`.

2. The convolution-memory-not-freed issue (9.22.0 KI) would become medium-severity
   if the fork ever ships a long-running inference server. Track in `docs/state.md`
   as a deferred potential bug; reopen when server-mode is implemented (VMAFX Phase 3
   cloud-native plan, ADR-0714 area).

3. The cuBLASLt perf regression with CUDA 13.2 Update 1 is distinct from the cuDNN
   layer and affects any matmul regardless of cuDNN; monitor when the Containerfile
   CUDA pin advances beyond 13.2.

---

## 6. Reproducer / verification commands

```bash
# Confirm CPU-only ORT tarball is used (no 'gpu' in filename):
grep 'ORT_VERSION\|onnxruntime-linux' dev/Containerfile

# Confirm no onnxruntime-gpu dependency in Python packages:
grep -rn 'onnxruntime-gpu\|onnxruntime-cuda' ai/ pyproject.toml python/ tools/

# Confirm no cuDNN apt install in Containerfile:
grep 'libcudnn\|cudnn' dev/Containerfile

# If investigating GPU EP: check ORT CUDA compatibility table at
# https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html
```

---

## 7. References

- ORT 1.26.0 CPU tarball: `github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-linux-x64-1.26.0.tgz`
- ORT CUDA EP compat table: `https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html`
- cuDNN 9.22.0 release notes: `https://docs.nvidia.com/deeplearning/cudnn/backend/latest/release-notes.html`
- cuDNN downloads: `https://developer.nvidia.com/cudnn-downloads`
- Containerfile: `dev/Containerfile` lines 529–539 (ORT), lines 146–171 (CUDA 13.2)
- DNN backend: `core/src/dnn/ort_backend.c`, `core/src/dnn/ort_backend.h`
- Quantisation script comment: `ai/scripts/measure_quant_drop_per_ep.py` lines 17–18
