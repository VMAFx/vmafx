<!-- markdownlint-disable MD013 -->
# Tiny AI — overview

The **Tiny AI** surface lets you ship small, specialized perceptual-quality
models alongside the classic VMAF SVM without introducing a second ML
runtime or giving up libvmaf's C-only deployment story. The whole feature
is gated on `-Denable_dnn=auto|enabled|disabled` (default `auto`) and
consumed through a single public header, `libvmaf/dnn.h`.

## The four capabilities

| # | Name | Shape | What you do with it |
| --- | --- | --- | --- |
| **C1** | FR regressor | feature-vector → MOS | Replace / augment the upstream `vmaf_v0.6.1` SVM with a lightweight MLP trained on your own reference-dataset. |
| **C2** | NR metric | frame → MOS | Predict quality without a reference (live encodes, consumer telemetry). |
| **C3** | Learned filter | frame → frame | Denoise / deblock / sharpen before encoding to squeeze out VMAF/PSNR budget. |
| **C4** | LLM dev helpers | repo-time only | Review, commit-msg drafting, docgen — never linked into libvmaf. |

C1/C2/C3 share **one** runtime (ONNX Runtime C API). C4 is independent and
lives in [`dev-llm/`](../../dev-llm/).

## How the pieces fit together

```text
       ┌─────────────────────────┐
       │        ai/              │  torch + lightning + typer
       │  (train / export / reg) │  → ONNX
       └────────────┬────────────┘
                    │  .onnx + sidecar .json
                    ▼
       ┌─────────────────────────┐
       │  model/tiny/ (git-lfs)  │  committed tiny models
       └────────────┬────────────┘
                    │
                    ▼
   ┌──────────────────────────────────────┐
   │     core/src/dnn/  (C, ORT)       │   one runtime
   │     vmaf_use_tiny_model(...)         │   shared by all surfaces
   └──────────────┬────────────┬──────────┘
                  │            │
                  ▼            ▼
           vmaf CLI       ffmpeg vf_libvmaf  +  vf_vmaf_pre
```

**Key invariant.** Training lives in Python and depends on PyTorch +
Lightning. Runtime lives in C and depends only on ONNX Runtime. The
boundary is the `.onnx` + sidecar JSON pair on disk.

## Runtime availability

Tiny-AI extractors (`lpips`, `dists_sq`, `fastdvdnet_pre`, `mobilesal`,
and `transnet_v2`) require a libvmaf build with ONNX Runtime support. On a
build compiled with `-Denable_dnn=disabled`, extractor `init` returns
`-ENOSYS` before probing `model_path` or the extractor-specific environment
variable. On DNN-enabled builds, a missing model path remains a normal
configuration error and returns `-EINVAL`.

## When to reach for Tiny AI

| You want to… | Use | Read |
| --- | --- | --- |
| Beat the upstream SVM's PLCC on your own MOS data | C1 | [training.md](training.md) |
| Score VMAF without a reference | C2 | [inference.md](inference.md) |
| Pre-filter frames before encoding | C3 | [inference.md](inference.md) |
| Compare a new model's PLCC/SROCC/RMSE to the SVM baseline | — | [benchmarks.md](benchmarks.md) |
| Understand the operator allowlist + signature model | — | [security.md](security.md) |

## Related documents

- [roadmap.md](roadmap.md) — Wave 1 scope expansion (LPIPS, saliency, per-shot CRF, `vmaf_post`, allowlist `Loop`/`If`, MCP VLM tool).
- [training.md](training.md) — `vmaf-train` CLI, dataset manifests, export flow.
- [inference.md](inference.md) — CLI / C API / ffmpeg filter surfaces.
- [benchmarks.md](benchmarks.md) — accuracy + throughput methodology.
- [security.md](security.md) — operator allowlist, size cap, Sigstore verification.

## Per-model reference

Every shipped tiny-AI checkpoint gets its own usage page under
[`models/`](models/) — see [CLAUDE.md §12 rule 10 / ADR-0042](../adr/0042-tinyai-docs-required-per-pr.md)
for the rule. Current pages:

- [LPIPS-SqueezeNet](models/lpips_sq.md) — full-reference perceptual distance.
- [DISTS-Sq](models/dists_sq.md) — full-reference DISTS-shaped smoke checkpoint.
- [fr_regressor_v1](models/fr_regressor_v1.md) — FR MLP regressor v1 (baseline).
- [fr_regressor_v2](models/fr_regressor_v2.md) — FR MLP regressor v2.
- [fr_regressor_v2 codec-aware](models/fr_regressor_v2_codec_aware.md) —
  codec-aware variant.
- [fr_regressor_v2 probabilistic](models/fr_regressor_v2_probabilistic.md) —
  uncertainty-aware variant.
- [fr_regressor_v3](models/fr_regressor_v3.md) — FR MLP regressor v3.
- [vmaf_tiny_v2](models/vmaf_tiny_v2.md), [vmaf_tiny_v3](models/vmaf_tiny_v3.md),
  [vmaf_tiny_v4](models/vmaf_tiny_v4.md), [vmaf_tiny_v5](models/vmaf_tiny_v5.md)
  — progressive VMAF-tiny checkpoint series.
- [saliency_student_v1](models/saliency_student_v1.md),
  [saliency_student_v2](models/saliency_student_v2.md) — saliency map heads.
- [fastdvdnet_pre](models/fastdvdnet_pre.md) — temporal pre-filter (5-frame window).
- [mobilesal](models/mobilesal.md) — lightweight saliency extractor.
- [transnet_v2](models/transnet_v2.md) — shot-boundary detector (~1M params).
- [KoNViD MOS head v1](models/konvid_mos_head_v1.md) — NR MOS head trained on KoNViD-1k.
