<!-- markdownlint-disable MD060 -->
# `vmaf-tune --score-backend` — GPU acceleration of the scoring loop

`vmaf-tune corpus` and `vmaf-tune per-shot` invoke the
[`vmaf`](cli.md) CLI for every grid cell to compute the VMAF score
of an encoded variant against the reference. By default the score
loop runs on the CPU, which dominates wall time on long sweeps.
`--score-backend` selects an accelerated backend for the score axis
without changing the encode axis at all.

The flag is governed by [ADR-0299](../adr/0299-vmaf-tune-gpu-score.md)
(initial CUDA / SYCL / CPU shape),
[ADR-0314](../adr/0314-vmaf-tune-score-backend-vulkan.md) (Vulkan
addition, now superseded by ADR-0726), and
[ADR-0667](../adr/0667-vmaf-tune-score-backend-native-priority.md)
(HIP/ROCm and native-first `auto` ordering).
The Vulkan backend was removed in [ADR-0726](../adr/0726-drop-vulkan-backend.md).

## Usage

```shell
vmaf-tune corpus \
    --source ref.yuv --width 1920 --height 1080 \
    --pix-fmt yuv420p --framerate 24 \
    --encoder libx264 --preset medium --crf 22 --crf 28 \
    --score-backend cuda \
    --out corpus.jsonl
```

## Accepted values

| Value  | Behaviour                                                                                                                                                                                                          |
|--------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `auto` | (default) probe the host and pick the fastest available, in the order `cuda → sycl → hip → cpu`.                                                                                                                   |
| `cuda` | NVIDIA GPU score path. Errors out if the local `vmaf` was built without CUDA, or if `nvidia-smi` is missing / reports no devices.                                                                                  |
| `sycl` | Intel oneAPI SYCL path. Errors out if `vmaf` was built without SYCL or `sycl-ls` reports no devices.                                                                                                               |
| `hip`  | AMD ROCm score path. Errors out if the local `vmaf` was built without HIP, or if `rocminfo` / `rocm-smi` cannot see an AMD GPU.                                                                                    |
| `cpu`  | Force CPU scoring. Always available; useful as a baseline reference when chasing a numeric divergence.                                                                                                             |

> **Note**: The `vulkan` backend was removed in [ADR-0726](../adr/0726-drop-vulkan-backend.md).
> Passing `--score-backend vulkan` now raises an error at startup.

## Hard-failure semantics

A non-`auto` value is **strict**: an explicit `--score-backend cuda`
on a host without CUDA fails with a clear error rather than silently
falling back. This avoids the most common GPU-pipeline footgun —
running a "GPU sweep" that quietly produced CPU numbers because the
GPU wasn't actually engaged.

`auto` walks the fallback chain (`cuda → sycl → hip → cpu`) and
picks the first entry that is **both** advertised by the local `vmaf`
binary's `--help` *and* probe-confirmed via the appropriate vendor
tool. The probe results print to stderr at log-level INFO so a
post-hoc reader can see which backend was selected.

> **Note**: The `vmaf-tune` probe order (`cuda → sycl → hip → cpu`) differs
> from the libvmaf internal registry order (`sycl → cuda → hip → cpu`). The
> vmaf-tune ordering was set by
> [ADR-0667](../adr/0667-vmaf-tune-score-backend-native-priority.md) to probe
> CUDA first because it has the strongest vendor tooling for availability
> detection (`nvidia-smi`). The libvmaf registry prioritises SYCL first
> because Intel Arc is the primary continuous-integration GPU. When comparing
> timing results across backends, be aware that `auto` may select different
> backends in vmaf-tune versus a direct `vmaf --backend auto` invocation.

## Cross-backend numeric drift

Per the project's GPU-parity gate ([ADR-0214](../adr/0214-gpu-parity-ci-gate.md)),
GPU score backends produce results that are *close* to the CPU floor
but **not bit-identical**. The CPU is the numerical-correctness
floor — for a strict numeric comparison run the same corpus twice,
once with `--score-backend cpu` and once with `--score-backend
<gpu>`, then diff the JSONL `vmaf_score` column. Typical drift is
under 1e-3 VMAF points on 1080p / 24 fps content; the precise
per-feature ULP envelopes live in
[`docs/development/cross-backend-gate.md`](../development/cross-backend-gate.md).

## Performance

The GPU score path is ~10–30× faster than the CPU path on 1080p / 24
fps content (the roughly-quoted figure from the CUDA backend's
header docstring). The encode path is untouched by this flag — if
the encode dominates wall time (e.g. AV1 with `libsvtav1 --preset
0`), enabling a GPU score backend has only marginal effect on total
sweep time.

## Implementation notes

- The flag is wired in `tools/vmaf-tune/src/vmaftune/cli.py` and
  resolves to a concrete backend via
  `tools/vmaf-tune/src/vmaftune/score_backend.py`'s `select_backend`.
- The resolved backend is passed to the underlying `vmaf` invocation
  as `--backend <name>`; the libvmaf-side selector is per-feature
  per [ADR-0212](../adr/0212-hip-backend-scaffold.md).
- For one-shot single-encode scoring, the same `--backend` flag is
  available on the `vmaf` CLI directly — see
  [`docs/usage/cli.md`](cli.md).

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md) — the
  encoder side (the `--score-backend` flag is orthogonal to
  `--encoder`).
- [`docs/backends/cuda/overview.md`](../backends/cuda/overview.md) /
  [`sycl/overview.md`](../backends/sycl/overview.md) /
  [`hip/overview.md`](../backends/hip/overview.md) — backend
  build / runtime requirements.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md)
  — audit that triggered this page.
