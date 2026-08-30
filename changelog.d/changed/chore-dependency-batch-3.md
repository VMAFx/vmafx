- Third dependency batch: `typer`, `onnxruntime`, `openai` and `ruff`. Batched
  into one branch and one CI run for the reason in ADR-1123 — a per-PR fan-out of
  the full macOS/ARM/CUDA/SYCL matrix is disproportionate for one-line manifest
  edits, and the merge gate cannot drain it.
