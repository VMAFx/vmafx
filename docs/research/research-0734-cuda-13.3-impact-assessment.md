<!-- markdownlint-disable MD013 MD029 MD060 -->
# Research-0734: CUDA 13.3 impact assessment for the VMAFX fork

- **Status**: Active
- **Workstream**: CUDA toolkit bump scoping; vmafx-server inference path; tiny-AI DNN runtime
- **Last updated**: 2026-05-28

---

## Executive summary

CUDA 13.3 introduces three things relevant to this fork: **Tile C++** (a new
intra-block parallelism language extension that the compiler auto-parallelises),
**CompileIQ** (an opt-in evolutionary autotuner for internal compiler
parameters, not a transparent flag), and **CUDA Python 1.0 green contexts**
(runtime-exposed SM-partitioning, generally available since CUDA 13.1 via the
new Execution Context abstraction; driver-API-only in 12.4–13.0). Of these,
only Tile C++ has a plausible "rewrite our kernels for a measurable win" story
— and even that is bounded: NVIDIA's announcement post ships **no migration
guidance and no benchmark numbers**, and the much-cited "15% speedup" headline
is **CompileIQ on Meta's TritonBench/Helion kernels reported by Meta**, not a
first-party CUDA-kernel measurement. **Net recommendation for the toolkit
bump**: bump the floor to CUDA 13.3 once NVCR/Jimver publish images (gating
item, no ETA verified), pilot CompileIQ on `integer_vif_score/filter1d.cu`
(largest hot kernel, 845 LOC) and `integer_adm/adm_cm.cu` (382 LOC) as a
build-time `--apply-controls` experiment without touching kernel source, and
**defer** any Tile-C++ rewrite until a downstream benchmark (TritonBench-style)
on a representative VMAF kernel exists. Green contexts are interesting for
vmafx-server multi-tenant SM partitioning but the official docs explicitly
warn concurrent execution is **not guaranteed**, and the only Python
ML-framework binding we'd realistically reuse (Torch-TensorRT) currently
silently bypasses caller-supplied streams (PR #4232 open as of 2026-05-27),
so the integration is not turnkey today.

---

## Per-item recommendations

| # | Item                                          | Recommendation         |
|---|-----------------------------------------------|------------------------|
| 1 | Tile-C++ rewrite of CUDA feature kernels      | **wait-for-benchmark** |
| 2 | CompileIQ autotune on hottest kernels         | **do-now (pilot)**     |
| 3 | Green contexts for vmafx-server isolation     | **wait-for-Torch-TRT**  |
| 4 | DLPack tensor accept in ONNX Runtime path     | **not-worth-it (yet)** |
| 5 | CUDA 13.3 image availability in CI            | **wait-for-NVCR**      |

---

## 1. Tile C++ rewrite candidates — wait-for-benchmark

### 1.1 What Tile C++ actually is

CUDA 13.3 ships **CUDA Tile C++** as an official language extension expressing
intra-block parallelism through tile abstractions; the compiler
auto-parallelises tile code across threads and utilises TMA / Tensor Cores
where available
(`docs.nvidia.com/cuda/cuda-tile-cpp-api-reference`,
`developer.nvidia.com/blog/develop-high-performance-gpu-kernels-in-cpp-with-nvidia-cuda-tile/`).
It is supported on CC 9.0 (Hopper) and "all other supported GPU architectures"
per the announcement post.

Migrating a SIMT kernel to a tile kernel is **not a refactor** — it is a
rewrite. Required syntactic/structural changes (all verified verbatim from
NVIDIA's blog and API reference):

- `__tile_global__` replaces `__global__` annotation
- `ct::tensor_span{pointer, ct::extents{...}}` wrappers analogous to C++23 mdspan
- `ct::partition_view{span, ct::shape{...}}` replaces explicit thread indexing
- `load_masked` / `store_masked` for tile data movement with implicit
  boundary checks (no manual `if (idx < N)` guards)
- `ct::bid()` (destructured as `auto [xBlock, yBlock, _] = ct::bid();`) for
  block indices

### 1.2 Candidate kernels in our tree

Measured against `core/src/feature/cuda/` (live LOC at 2026-05-28):

| Kernel file                                      | LOC | Pattern                                | Tile-C++ fit | Migration cost |
|--------------------------------------------------|-----|----------------------------------------|--------------|----------------|
| `integer_vif/filter1d.cu`                        | 845 | 1-D separable filter, multi-scale loop | **high**     | high (~2 wk)   |
| `integer_adm/adm_cm.cu`                          | 382 | 2-D contrast masking, tile reductions  | **high**     | medium (~1 wk) |
| `integer_adm/adm_dwt2.cu`                        | 348 | 2-D DWT lifting, intra-tile dataflow   | **high**     | medium (~1 wk) |
| `integer_ssim/integer_ssim_score.cu`             | 273 | 2-D windowed stats (mu/sigma/cov)      | **high**     | medium (~1 wk) |
| `integer_adm/adm_decouple.cu`                    | 230 | per-pixel orientation decision         | medium       | low (~3 d)     |
| `integer_motion/motion_score.cu`                 | 209 | abs-diff + reduction                   | medium       | low (~3 d)     |
| `integer_ssim/ssim_score.cu`                     | 208 | post-process reduction                 | low          | low (~2 d)     |
| `integer_adm/adm_csf.cu`                         | 208 | per-band CSF multiply                  | low          | low (~2 d)     |
| `integer_ms_ssim/ms_ssim_score.cu`               | 209 | multi-scale aggregate                  | low          | low (~2 d)     |
| `integer_adm/adm_csf_den.cu`                     | 137 | reduction                              | low          | low (~2 d)     |

Total LOC in the candidate set: **3 049 LOC**. The two largest kernels
(`filter1d.cu` and `adm_cm.cu`) account for 40 % of the surface and are also
the strongest structural matches for tile semantics (separable filter with
implicit boundary handling, 2-D windowed reduction).

### 1.3 Why "wait-for-benchmark" and not "do-now"

- **No published benchmark numbers** for Tile C++ on anything resembling our
  workload. NVIDIA's two CUDA-13.3 posts describe the API surface and the
  "automatically manages low-level GPU details" framing but **do not publish
  speedup numbers**; the "up to 15 %" figure ubiquitously quoted is from
  CompileIQ on Meta TritonBench/Helion kernels, not Tile C++.
- **Bit-exactness risk**: our CUDA backend is NOT bit-identical to CPU
  (memory rule: "GPUs NOT bit-exact"), but per-backend snapshots in
  `testdata/scores_cpu_*.json` and `netflix_benchmark_results.json` gate
  drift. A compiler-managed tile rewrite changes intra-block reduction
  order and thus the snapshot — each candidate above needs a deliberate
  `/regen-snapshots` with justification.
- **Bit-exact-vs-scalar invariants** in some kernels (e.g.,
  `integer_adm/*`) constrain reduction order at the SIMD-DX framework
  level; the same constraints carry into the tile rewrite and reduce the
  optimiser's room.
- The migration is a **rewrite, not a port** — every `__global__` becomes
  `__tile_global__`, every loop disappears into tile operations, every
  manual boundary check is rewritten as `load_masked`/`store_masked`. Cost
  is bounded by the unfamiliarity of the API surface (no in-tree precedent)
  more than the LOC count.

**Pilot proposal** (if/when we move): rewrite ONLY `integer_vif/filter1d.cu`
as a single feature-flagged path (`-Denable_cuda_tile=true`), gate behind
cross-backend-diff, measure on RTX 4090 + Hopper, and decide propagation
based on the measured delta. Estimated single-kernel cost: **~2 weeks
engineer time + ~1 week snapshot/parity cycle**.

---

## 2. CompileIQ — do-now (pilot)

### 2.1 What it actually tunes (and does not)

CompileIQ ships in CUDA 13.3, installable via `pip install compileiq` (PyPI
v1.0.0, published 2026-05-26 by NVIDIA, Python 3.11-3.13). It tunes **internal
compiler parameters that are not exposed via public flags**:

- register allocation strategies
- instruction scheduling policies
- loop transformations
- "and more" (NVIDIA's wording — no exhaustive list)

It does **NOT** tune block size, grid shape, or any kernel-launch parameter —
those are runtime concerns out of the compiler's scope. ILP is influenced
*indirectly* via scheduling and register allocation but is not a directly
tuned dimension.

### 2.2 It is NOT transparent

The "auto" in autotune is misleading. To use CompileIQ, the developer must:

1. Install the `compileiq` Python package.
2. Write an **objective function** — a Python callable that takes a candidate
   compiler configuration, compiles the kernel, benchmarks it, returns a
   score.
3. Configure `SearchConfiguration` (pool_size, cull_size, generations,
   mutate_rate) for an evolutionary search.
4. Run the search until convergence.
5. Deploy the result by invoking NVCC 13.3+ with
   `--apply-controls=<file>.acf`, where the ACF is the Advanced Controls
   File produced by step 4.

NVCC docs warn: "Using an advanced controls file may cause compilation
failure or incorrect runtime execution." So ACFs are per-kernel,
per-architecture, and need re-validation against our bit-exactness
snapshots.

### 2.3 The 15 % claim — source and methodology

The headline figure originates from a **Meta GTC talk** measuring CompileIQ
on Meta's own **TritonBench** and **Helion** kernel suites. NVIDIA's exact
wording on the CompileIQ blog: "Meta has seen up to 15 % performance
improvement on both TritonBench and Helion kernels as shown in this GTC talk."
Implications for us:

- "Up to 15 %" is the upper bound on already-Triton/CUTLASS-optimised GEMM
  and attention kernels, not arbitrary CUDA kernels.
- The **CompileIQ landing page itself publishes no quantified numbers** —
  the figure lives only on the blog and in the cited GTC talk.
- Our VMAF kernels are mostly stencils/filters/reductions, not GEMM/attn —
  the closest analogue in CompileIQ's published evidence is "loop
  transformations help kernels with regular loop structures", which fits
  `filter1d.cu` and `adm_dwt2.cu` better than `motion_score.cu`.

### 2.4 Why "do-now (pilot)"

CompileIQ is **build-time only** (no kernel-source change), opt-in via
`--apply-controls`, and the worst-case downside is "ACF produces broken
binary, we fall back to default NVCC". That risk profile fits a pilot:

- **Pilot target**: `integer_vif/filter1d.cu` (largest kernel, regular-loop
  structure, most LOC under tuning influence).
- **Cost**: ~1 week to author the objective function + benchmark harness
  (CHUG corpus subset on RTX 4090).
- **Output**: one ACF checked in per `(kernel, sm_arch)` tuple, applied via
  Meson custom target, validated against bit-exact-vs-snapshot.
- **Decision gate**: if the pilot returns < 3 % improvement on the Netflix
  benchmark replay (testdata/netflix_benchmark_results.json), drop CompileIQ
  from the toolkit-bump scope; if ≥ 3 %, propagate to `adm_cm.cu` next.

CompileIQ is a free upside that does not block the toolkit bump — wire it
behind a Meson option (`-Denable_cuda_compileiq=false` default) so it can
live as opt-in until we have measurements.

---

## 3. vmafx-server green contexts — wait-for-Torch-TRT

### 3.1 What green contexts give us

Green contexts (CUDA Programming Guide §4.6) partition GPU resources — SMs
and work queues — at context creation time, so a GC's kernels are restricted
to its provisioned SMs. They became Driver-API-available in CUDA 12.4 and
are exposed in the **CUDA runtime** via the Execution Context (EC)
abstraction starting in **CUDA 13.1**. CUDA Python 1.0 wraps them on the
Python side.

For our `cmd/vmafx-server/` (Go + cgo libvmaf + ONNX Runtime), the
hypothetical benefit is **per-request SM partitioning**: shield a
latency-sensitive small-batch inference RPC from a long-running
batch-scoring RPC on the same GPU within the same process.

### 3.2 The hard "but"

NVIDIA's official guide carries an explicit Attention callout:

> "Even when different SM resources and work queues are provisioned per
> green context, concurrent execution of independent GPU work is **not
> guaranteed**."

(`docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/green-contexts.html`)

So green contexts are a **scheduling hint, not a hardware isolation
primitive**. The contrast with MIG (A100 GPC partitioning, hard isolation,
predictable per-partition throughput) is meaningful — MIG-class isolation
is not what green contexts deliver in the same-process case.

### 3.3 The Torch-TensorRT bug

If we ever route inference through Torch-TensorRT (current vmafx-server
uses ORT directly, but the FX/Triton tiny-AI path could in future), the
caller's CUDA stream is **silently substituted with a pool stream that is
not bound to any green context**, so SM partitioning is bypassed entirely.
Source: pytorch/TensorRT PR #4232 (open, filed 2026-05-01, last activity
2026-05-27). The PR's own description: "Torch-TensorRT cannot be used for
GPU partitioning at all today."

Our current vmafx-server uses ORT-C-API via cgo, not Torch-TensorRT, so
PR #4232 doesn't directly bite us — but the broader pattern (Python
ML-framework runtimes silently substituting streams) is a known pothole.
Before plumbing green contexts into the Go server, audit ORT's CUDA
execution provider for the same pattern.

### 3.4 "One process per request" comparison

The current pattern (process-per-request, OS-scheduled) gives us:

- Hard memory isolation (separate processes)
- Predictable scheduling (kernel-level CFS)
- No SM-level partitioning (each process has the full GPU when scheduled)

Green contexts within a single process would give us:

- No memory isolation (same address space)
- SM-level partitioning hints (not guarantees)
- Lower per-request cost (no process spawn)

For our latency profile (scoring requests are O(100 ms) and dominated by
H2D/D2H + kernel time, not process spawn), the wins are marginal.
**Defer until** (a) we have a workload where process-spawn is the bottleneck
and (b) the Torch-TensorRT-class stream-bypass bugs are surveyed in our
actual runtime stack.

---

## 4. DLPack in ONNX Runtime — not-worth-it (yet)

### 4.1 Current state

Grep of `core/src/dnn/` for `DLPack|dlpack|DLManagedTensor` returns
**zero hits** — our ORT integration goes through `tensor_io.c` /
`ort_backend.c` with explicit `OrtValue` allocation and host/device copies.

### 4.2 The published DLPack-in-ORT history

A long-standing microsoft/onnxruntime issue (#15963) tracked
DLPack-protocol support being restricted to *training* builds, not the
main *inference* build. The adversarial verification round for this digest
**refuted** the historically-cited blockers (DLPack restricted to training
builds, .numpy() round-trip required, issue closed without resolution) —
those claims could not be independently verified at the level the scoping
needs. We cannot assert ORT inference cannot accept DLPack today; we also
cannot assert it can.

### 4.3 Why "not-worth-it (yet)"

- Our callers are **vmafx-server (Go)** and **MCP (Python)** — neither
  hands us a DLPack-wrapped device tensor today. Both produce decoded
  frame buffers as host-side `uint8_t*` from FFmpeg.
- Zero-copy benefit would require the *caller* to hand us a device tensor
  (PyTorch / Triton) — which our current architecture does not.
- The closest win is the Phase-4b distributed-platform path (eBPF +
  sidecar training, see project memory) where a Triton sidecar might
  produce features as `cuda.cupy` tensors; that pattern is months away,
  not now.
- Refresh this assessment when (a) the tiny-AI training sidecar lands and
  (b) ORT inference-build DLPack support is independently verified
  against the version we ship.

---

## 5. CUDA 13.3 image availability — wait-for-NVCR

### 5.1 The two upstream dependencies

Our CI / dev-container pipeline pulls CUDA from two places:

1. **NVCR `nvidia/cuda:<version>-devel-ubuntu24.04`** — base image for
   `dev/Containerfile`.
2. **`Jimver/cuda-toolkit` GitHub Action** — used in `.github/workflows/*`
   for Windows / Linux runners that build CUDA-enabled artifacts.

Both are external; neither has a published ETA that survived the
adversarial verification round.

### 5.2 What we know

- CUDA 13.3 is **released by NVIDIA** (NVIDIA's developer.nvidia.com blog
  posts dated within the May 2026 window, CompileIQ on PyPI 2026-05-26).
- Historical lag from NVIDIA GA to first NVCR image: typically 2–4 weeks
  for `.0` releases, 1–2 weeks for `.x` point releases.
- Historical lag for `Jimver/cuda-toolkit`: bounded by the maintainer's
  PR cycle, typically 1–3 weeks after the NVIDIA installer ships.

### 5.3 Why "wait-for-NVCR"

We do not have a verified ETA for either dependency. The toolkit bump
**cannot land** until both are available — building locally against host
CUDA 13.3 is fine for dev, but CI will be red. Mitigation:

- Open a watch issue, file weekly NVCR / Jimver checks until both publish
  13.3.
- In the meantime, the CompileIQ pilot can run on host-installed CUDA 13.3
  out-of-tree without blocking the merge train.

---

## 6. Toolkit-bump merge sequencing

When both upstream images are available:

1. Update `dev/Containerfile` base image tag → `nvidia/cuda:13.3.0-devel-ubuntu24.04`.
2. Update `Jimver/cuda-toolkit@vN` action version pin + CUDA version input.
3. Rebuild dev-mcp container, validate against `testdata/scores_cpu_*.json`
   and `netflix_benchmark_results.json` (any drift = bit-exactness
   regression, gate hard).
4. Land Meson option `-Denable_cuda_compileiq=false` (default off).
5. Land the CompileIQ pilot ACF for `filter1d.cu` behind that option.
6. Open separate ADR for Tile-C++ pilot ONLY after a real benchmark exists.

Estimated wall-time once both images publish: **~1 week** for steps 1-3,
+1 week for step 5.

---

## References

### Primary (NVIDIA)

1. NVIDIA Developer Blog — *Develop High-Performance GPU Kernels in C++
   with NVIDIA CUDA Tile*.
   `developer.nvidia.com/blog/develop-high-performance-gpu-kernels-in-cpp-with-nvidia-cuda-tile/`
2. NVIDIA Developer Blog — *NVIDIA CUDA 13.3 Enhances GPU Development with
   Tile Programming in C++, Compiler Autotuning, and Python Updates*.
   `developer.nvidia.com/blog/nvidia-cuda-13-3-enhances-gpu-development-with-tile-programming-in-c-compiler-autotuning-and-python-updates/`
3. NVIDIA Developer Blog — *Extract More Kernel Performance with NVIDIA
   CompileIQ Auto-Tuning*.
   `developer.nvidia.com/blog/extract-more-kernel-performance-with-nvidia-compileiq-auto-tuning/`
4. NVIDIA Docs — *CUDA Tile C++ API Reference*.
   `docs.nvidia.com/cuda/cuda-tile-cpp-api-reference/index.html`
5. NVIDIA Docs — *CUDA Programming Guide §4.6 Green Contexts*.
   `docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/green-contexts.html`
6. NVIDIA Developer — *CompileIQ landing page*.
   `developer.nvidia.com/cuda/compileiq`

### Secondary

7. PyPI — `compileiq` 1.0.0 (published 2026-05-26).
8. pytorch/TensorRT PR #4232 — *Honor caller's CUDA stream so green-context
   partitioning works* (open, 2026-05-01).

### Internal

9. `core/src/feature/cuda/` — live LOC measurements (2026-05-28).
10. `core/src/dnn/` — current ORT integration (no DLPack).
11. `cmd/vmafx-server/` — current Go + cgo libvmaf + ORT inference path.
12. Project memory — `feedback_golden_gate_cpu_only.md` (GPU snapshots gate
    drift, not bit-exactness vs CPU).
