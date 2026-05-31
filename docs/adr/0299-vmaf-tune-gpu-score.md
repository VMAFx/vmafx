<!-- markdownlint-disable MD060 -->
# ADR-0299: GPU scoring backend for `vmaf-tune` (`--score-backend`)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, cuda, vulkan, sycl, ai, automation, fork-local

## Context

`vmaf-tune` (Phase A; [ADR-0237](0237-quality-aware-encode-automation.md))
drives an `(encoder, preset, crf)` grid sweep, encoding each cell with
FFmpeg and scoring the encode against the reference with the libvmaf
CLI. On a 60-second 1080p source, CPU-only VMAF scoring runs at
1–2 fps — the *score* axis dominates the corpus wall-clock once
encodes parallelise.

The fork already ships GPU-accelerated scoring backends:

- CUDA ([ADR-0127](0127-vulkan-compute-backend.md) sibling, mature):
  10–30 fps at 1080p depending on GPU class.
- Vulkan ([ADR-0175](0175-vulkan-backend-scaffold.md),
  [ADR-0186](0186-vulkan-image-import-impl.md)): comparable to CUDA on
  recent NVIDIA / AMD silicon.
- SYCL/oneAPI: Intel-first, comparable on Arc / Iris Xe.

The libvmaf CLI exposes them via the unified `--backend NAME` selector
(values: `auto|cpu|cuda|sycl|vulkan`). Until this ADR, `vmaf-tune`
invoked the CLI without `--backend`, leaving the binary in its built-in
auto-mode — which is *not* the same as actively detecting the host's
fastest backend, and was effectively CPU on workstations where the GPU
backends' auto-engagement heuristics don't fire (e.g. when neither
`--gpumask` nor `--sycl_device` is set).

The user-facing speedup is 10–30× on score wall-clock, which translates
directly to corpus throughput once encodes are no longer the long pole.

## Decision

We add a `--score-backend {auto|cpu|cuda|sycl|vulkan}` flag to
`vmaf-tune corpus` (default `auto`) that resolves to a libvmaf
`--backend NAME` argument before any encodes run.

Selection logic lives in a new module
`tools/vmaf-tune/src/vmaftune/score_backend.py`:

- `parse_supported_backends(help_text)` extracts the alternation from
  the vmaf binary's `--help` output. CPU is always considered
  supported.
- `detect_available_backends()` intersects binary support with cheap
  hardware probes (`nvidia-smi -L`, `vulkaninfo --summary`,
  `sycl-ls`).
- `select_backend(prefer)` honours the user's choice. `auto` walks the
  fallback chain (`cuda → vulkan → sycl → cpu`) and returns the first
  available; any other value is treated **strictly** — if the
  requested backend is not available, raise `BackendUnavailableError`
  with a diagnostic message. We never silently downgrade an explicit
  GPU request to CPU; that would mask hardware/build mismatches and
  lie to the operator about wall-clock expectations.

`run_score` and `build_vmaf_command` accept an optional
`backend` kwarg that, when set, appends `--backend NAME` to the
spawned argv. `None` preserves legacy behaviour for callers that
haven't migrated.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Always inject `--backend auto` | One-liner change in `score.py` | The libvmaf `auto` heuristic is conservative and routes to CPU on hosts where the user *would* want CUDA but no `--gpumask` is set. No telemetry for which backend ran. | Doesn't actually deliver the 10–30× speedup the user wants by default. |
| Detect once globally, ignore user preference | Simplest CLI surface (no new flag) | Operators on multi-GPU CI runners need to pin a backend for reproducibility. No way to force CPU for a known-bad GPU driver day. | Loses the strict-mode guarantee (no silent downgrade). |
| Probe by **invoking** vmaf with each backend and keeping the survivor | Most accurate (catches runtime init failures, not just driver presence) | Spawns up to 4 subprocesses per `vmaf-tune` invocation — adds 5–10 s of cold start; doesn't compose with the Phase A "JSONL row per cell" mental model. | Too slow for an interactive selection step. |
| Parse `vmaf --capabilities` JSON | Most robust (no help-text parsing fragility) | The CLI doesn't expose a machine-readable capability dump yet; would require a libvmaf change. | Future work; ship the help-parser first, swap when available. |

## Consequences

- **Positive**:
  - 10–30× faster `vmaf-tune corpus` runs on GPU-equipped hosts; the
    score axis stops dominating wall-clock.
  - Strict-mode (`--score-backend cuda` on a CPU-only host) fails fast
    with a clear error — surfaces build/runtime mismatches the moment
    the operator hits them.
  - Auto-detection means default-on benefit for anyone with a working
    GPU; no flag-flip required.
- **Negative**:
  - Adds a (mockable) dependency on `nvidia-smi` / `vulkaninfo` /
    `sycl-ls` for the detection step. Missing tools degrade
    gracefully to "backend not available", never to a hard error.
  - Help-text parsing is fragile — if libvmaf renames `--backend` or
    reformats the `auto|cpu|cuda|sycl|vulkan` line, detection silently
    reports CPU-only. Mitigated by the unit tests in
    `tests/test_score_backend.py`, which pin the parser against a
    known-good help fragment.
- **Neutral / follow-ups**:
  - When libvmaf adds a machine-readable `--capabilities` dump, swap
    the help parser for that.
  - Phase B/C (`bisect`, `predict` per ADR-0237) inherit the same
    flag for free since they share `score.run_score`.

## References

- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune` umbrella.
- [ADR-0127](0127-vulkan-compute-backend.md) — Vulkan compute backend.
- [ADR-0175](0175-vulkan-backend-scaffold.md) — Vulkan scaffold.
- [ADR-0186](0186-vulkan-image-import-impl.md) — Vulkan image-import.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend numerical parity gate.
- Source: `req` — user requested wiring `vmaf-tune`'s scoring step to
  the existing CUDA/Vulkan/SYCL libvmaf backends, with the explicit
  hard rules "force-cuda on a host without CUDA must fail with a clear
  error" and "do not silently fall back to CPU if user explicitly
  requested GPU".
