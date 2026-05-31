<!-- markdownlint-disable MD013 -->
# Research-0071: `vmaf-tune` GPU score backend — option-space digest

- **Date**: 2026-05-03
- **Companion ADR**: [ADR-0299](../adr/0299-vmaf-tune-gpu-score.md)
- **Status**: Snapshot at proposal time.

## Question

`vmaf-tune corpus` (Phase A) bottlenecks on VMAF scoring at 1–2 fps on
1080p sources because it invokes the libvmaf CLI without engaging the
fork's existing GPU backends. The libvmaf CLI already exposes a unified
`--backend NAME` selector (`auto|cpu|cuda|sycl|vulkan`) shipped by
[ADR-0127](../adr/0127-vulkan-compute-backend.md) and friends. What's
the right way to wire that into `vmaf-tune`?

Three sub-questions:

1. How does the tool decide which backend the **local** vmaf binary
   actually supports (a CPU-only build of libvmaf does not advertise
   `--backend cuda`)?
2. How does the tool decide which backend the **host** can actually
   run (a CUDA-built vmaf on a host with no NVIDIA driver fails at
   runtime)?
3. What happens when the user explicitly asks for a backend the host
   can't deliver?

## Prior art surveyed

- **libvmaf CLI** (`core/tools/cli_parse.c`) — confirms `--backend`
  values and the `auto|cpu|cuda|sycl|vulkan` alternation in the help
  text. The help line is the cheapest binary-capability oracle.
- **Netflix/vmaf upstream** does not have a CLI-side backend selector
  (CPU-only). No prior art to inherit.
- **ffmpeg `-init_hw_device`** uses a "list, then select" two-step:
  `ffmpeg -init_hw_device list` enumerates, the user (or wrapper)
  picks. Closest analogue, but ffmpeg discovers via API not CLI.
- **PyTorch `torch.cuda.is_available()` / `torch.backends.mps.is_available()`** —
  in-process probe. Equivalent for our case would require calling
  libvmaf C-API at vmaf-tune startup; rejected as too heavy for a
  Python harness whose subprocess boundary is the integration seam
  (`AGENTS.md`).
- **Host probes** — settled on the standard tools:
  - `nvidia-smi -L` (NVIDIA driver probe, no compute work).
  - `vulkaninfo --summary` (Vulkan loader probe, lists devices).
  - `sycl-ls` (oneAPI tool, lists SYCL backends + devices).

## Decision-shaping observations

- **Help-text parsing is fragile but cheap.** The libvmaf CLI prints
  `--backend $name: ...auto|cpu|cuda|sycl|vulkan.` once. We pin
  parser behaviour with three test fixtures (full / cuda-only /
  no-backend-line) to catch reformats.
- **Hardware probes must degrade gracefully.** Missing `nvidia-smi`
  on a CUDA host (e.g. driver-less containers) is common; the probe
  returns "not available" rather than raising, so the rest of the
  fallback chain runs.
- **Strict-mode is a *correctness* requirement, not a UX choice.**
  Per task spec hard rule: an explicit `--score-backend cuda` on a
  host without CUDA must error out. Silent CPU fallback would lie
  about wall-clock expectations and mask build/runtime mismatches —
  the operator would think they got 30 fps when they actually got 1.

## Design choice

Two pure functions plus one I/O wrapper:

- `parse_supported_backends(help_text)` — pure string ops.
- `detect_available_backends(...)` — joins the help parse with
  three subprocess probes. Tests inject a fake `runner`.
- `select_backend(prefer, *, available, fallbacks)` — pure dispatch
  that either returns a backend name or raises
  `BackendUnavailableError`. Tests pass `available` literally.

This composes cleanly with the rest of `vmaf-tune`'s architecture
(subprocess boundary == test seam) and matches the existing patterns
in `score.py` / `encode.py`.

## Discarded alternatives

See ADR-0299 §"Alternatives considered" for the full table. Headline
rejections:

- **Always inject `--backend auto`**: doesn't engage GPU on hosts
  where libvmaf's auto heuristic stays conservative.
- **Probe by subprocess-invoking each backend**: 5–10 s startup tax,
  too slow.
- **Parse a future `vmaf --capabilities` JSON dump**: doesn't exist
  yet; ship the help-parser, swap when libvmaf gains the dump.

## Open follow-ups

- When libvmaf grows a machine-readable `--capabilities` output, swap
  the help parser. Track via a TODO referencing this digest.
- Phase B (`bisect`) and Phase C (`predict`) inherit the flag for
  free since they share `score.run_score`. No additional research
  needed at those phases.
