<!-- markdownlint-disable MD007 MD013 MD041 -->
<!--
  Workflow: port an existing feature extractor to a GPU backend
  (CUDA / SYCL / HIP / Vulkan). See `/add-gpu-backend` for the
  scaffold and ADR-0186 for the Vulkan image-import precedent.
-->
---
name: feature-extractor-port
description: Port a feature extractor to a GPU backend (cuda / sycl / hip / vulkan)
agent_type: general-purpose            # or vulkan-reviewer for VK PRs
isolation: worktree
worktree_drift_check: true
required_deliverables:

  - digest
  - alternatives
  - agents_md
  - reproducer
  - changelog
  - rebase_note
verification:
  netflix_golden: true                 # CPU-only — GPU twins compared via cross-backend
  cross_backend_places: 4              # places=4 ULP gate (ADR-0214)
  meson_test: true
forbidden:
  - modify_netflix_golden_assertions
  - lower_test_thresholds
  - skip_gpu_parity_gate               # T6-8 / ADR-0214 is a required CI status check
  - claim_gpu_bit_exact                # feedback_golden_gate_cpu_only: never claim CPU bit-exact
master_status_check: true
backlog_id: null                       # set to "T3-12", "T7-36", etc.

---

# Feature-extractor port — {{FEATURE}} → {{BACKEND}}

> **MUST RUN BEFORE DISPATCH**:
>
> ```bash
> scripts/ci/agent-eligibility-precheck.py --backlog-id "{{BACKLOG_ID}}"
> ```

## Worktree-isolation prelude

(see `_template.md`)

## Task

Port `{{FEATURE}}` to the **{{BACKEND}}** backend. Wire it into
runtime dispatch + register the kernel + add a smoke test.

Mandatory steps:

1. Read the scalar reference: `core/src/feature/{{FEATURE}}.c`.
2. Scaffold via `/add-gpu-backend` if {{BACKEND}} doesn't exist yet,
   otherwise add the kernel directly under
   `core/src/feature/{{BACKEND}}/`.
3. Register the new `vmaf_fex_{{FEATURE}}_{{BACKEND}}` symbol in
   the backend dispatch table.
4. Cross-backend ULP gate at `places=4`:

   ```bash
   scripts/ci/cross_backend_parity_gate.py \
       --feature {{FEATURE}} --backends cpu,{{BACKEND}} --places 4
   ```

5. Netflix golden gate (CPU-only — confirms the scalar isn't broken):

   ```bash
   make test-netflix-golden
   ```

## Constraints

- `feedback_golden_gate_cpu_only`: never claim GPU is bit-exact to
  CPU. The CPU golden is the ground truth; GPUs are gated at
  `places=4` ULP, which is "close" not "identical".
- ADR-0186: Vulkan image-import path requires the matching
  `ffmpeg-patches/` patch update in the same PR.
- CLAUDE.md §12 r14: any libvmaf C-API surface change requires the
  ffmpeg patch series replay against `n8.1` to pass.
- `feedback_no_test_weakening`: ULP threshold `places=4` is non-
  negotiable. Fix the kernel, not the gate.

## Deliverables

- Research digest under `docs/research/NNNN-{{FEATURE}}-{{BACKEND}}-port.md`
  with reduction-strategy choice (warp shuffle vs shared mem vs
  device atomic) + numerical-stability analysis.
- Decision matrix: alternative reduction strategies / image vs
  buffer descriptors / FP16 vs FP32 trade-off.
- AGENTS.md note in `core/src/feature/{{BACKEND}}/AGENTS.md`
  (create if absent) listing the new kernel file + parity-gate
  expectations.
- Changelog fragment under `changelog.d/added/`.
- Rebase note: only if the {{BACKEND}} runtime touched an
  upstream-mirror header. Otherwise `no rebase impact: fork-only
  backend kernel`.
- Reproducer: cross-backend parity-gate command + the matrix-table
  output showing `cpu vs {{BACKEND}}` worst-ULP.

## Return shape

- Branch name + PR URL.
- Cross-backend ULP table for `{{FEATURE}}` across all enabled
  backends.
- Kernel-source path: `core/src/feature/{{BACKEND}}/{{FEATURE}}_{{BACKEND}}.{c,cpp,cu}`.
