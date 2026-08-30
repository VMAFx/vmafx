# ADR-0240: GPU backend public-header pattern doc (PR3 of GPU dedup, doc-only)

- **Status**: Accepted
- **Date**: 2026-05-02
- **Deciders**: Lusoris
- **Tags**: docs, gpu, agents, fork-local

## Context

GPU dedup PR3 (after PR2 = ADR-0239 picture-pool extract) was originally
scoped as "GPU public header codegen — extract `libvmaf_{cuda,sycl,vulkan,hip}.h`
from one template". A 2026-05-02 audit measured the four headers and
found:

- ~20 of ~200 lines per header truly shared (state init / import_state
  / state_free lifecycle).
- The remaining 90 % is backend-specific feature surface — CUDA's
  picture preallocation, SYCL's DMABuf/VA/D3D11 import, Vulkan's
  VkImage zero-copy import + ring depth, HIP's scaffold-only
  surface.

Codegenning 10 % of each header would add a build-system Python
dependency (Jinja2 or hand-rolled emitter) for too little return — the
divergence between backends is the API surface, not the lifecycle
boilerplate. The dedup-via-pattern approach the tiny-AI extractor
template (ADR-0250) uses is a better fit: a *recipe* lives in `docs/`,
new backends consult it, no build-system additions.

## Decision

PR3 of the GPU dedup sequence ships **two documentation artefacts** instead
of a codegen pipeline:

1. [`docs/development/gpu-backend-template.md`](../development/gpu-backend-template.md)
   — the recipe new GPU backends follow (state lifecycle, optional
   sections for `_available` / `_list_devices` / picture preallocation
   / hwaccel zero-copy import, Doxygen + ABI conventions).
2. [`core/include/libvmaf/AGENTS.md`](../../core/include/libvmaf/AGENTS.md)
   — a public-headers-tree invariant note that points back at the
   template + pins the rebase-sensitive ordering of the four
   existing backend headers.

The picture-preallocation section enforces the SYCL/Vulkan
`NONE / HOST / DEVICE` three-method shape (CUDA's CUDA-specific
`HOST_PINNED` is documented as a historical quirk new backends
should not replicate). The "implementation MUST delegate to
`VmafGpuPicturePool`" rule (ADR-0239) is restated in the template
so future backends can't reimplement the round-robin lifecycle.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Doc-pattern (chosen)** | Mirrors the tiny-AI template precedent (ADR-0250); zero build-system dependency; agent-readable AGENTS guidance | Future backend additions rely on developer discipline (no enforcement) | Picked: 90 % of each header is genuinely backend-specific; codegen is the wrong tool for ~10 % shared shape |
| Jinja2 codegen at build time | Forces shared lifecycle into one template; new backends are a one-line spec addition | Adds a Python build dependency; obscures the headers (you edit the template, not the file); high friction for a 20-LOC dedup | Rejected: ROI doesn't justify the build-system addition |
| Pure-Python codegen with checked-in outputs | No build-time Python; same template + a `make regen-headers` target | Two sources of truth — easy to drift; requires a CI gate that re-renders + diffs | Rejected: same return as Jinja2 with extra drift surface |
| Thin shared header with macro-emit blocks | Pure C; no codegen; `#define X_BACKEND_LIFECYCLE(prefix)` style | C macro stamping for type names is fragile; readability collapses; Doxygen on macros is brittle | Rejected: cure worse than the disease |
| Skip PR3 entirely; jump to PR4 (kernel host-glue) | Less PR churn | Leaves the user's "headers second" sequence step unhonoured; no agent-readable pattern doc for the next GPU backend addition | Rejected: doc-pattern is small, real, and lands the sequence step |

## Consequences

- **Positive**:
  - New GPU backends (Metal, DirectML, future ROCm-replacement)
    have a written recipe to follow — same dedup pattern the
    tiny-AI extractors use (ADR-0250 / PR #251 → migrated in
    PR #265).
  - The four-header rebase invariant note pins which headers
    upstream syncs are *expected* to touch (only `libvmaf_cuda.h`)
    versus *mis-merge signals* (any of the other three).
  - The `NONE / HOST / DEVICE` three-method picture-preallocation
    convention is locked as the new-backend default;
    `HOST_PINNED` won't propagate beyond CUDA.
- **Negative**:
  - No machine-enforced rule that future backends consult the
    template. Discipline-only; relies on PR review + the AGENTS.md
    pointer firing in agents' contexts.
  - The dedup return is small in absolute LOC (template doc +
    AGENTS.md, ~250 lines added to ship the recipe).
- **Neutral / follow-ups**:
  - **PR4 of the dedup sequence** (T-GPU-DEDUP-3): GPU feature
    kernel host-glue extract. 10+ files per backend share
    state/init/close boilerplate (~250/500 LOC per file). The
    leverage there is dramatic; this is the highest-LOC dedup of
    the sequence.
  - **Vulkan picture_vulkan_pool.c migration** (T-GPU-DEDUP-2-vulkan):
    after #264 (Vulkan picture preallocation) lands, rewrite the
    Vulkan pool as a thin wrapper around the generic pool —
    parallel to what #266 did for SYCL. Tracked separately because
    it depends on #264 merging first.

## References

- Source: `req` 2026-05-02 — popup answer "all three sequenced —
  pool first, headers second, kernels third"; clarification reply
  "we only created the boilerplate but we didn't actually
  migrate" (which led to the audit-then-doc-pattern reframing of
  PR3 from codegen to recipe).
- 2026-05-02 GPU dedup audit (in-conversation, agent-produced).
- [ADR-0239](0239-gpu-picture-pool-dedup.md) — PR2 of the dedup
  sequence (picture-pool extract).
- [ADR-0250](0250-tiny-ai-extractor-template.md) — the precedent
  for "recipe doc + shared helpers, not codegen".
- [`docs/development/gpu-backend-template.md`](../development/gpu-backend-template.md)
  — the recipe.
- [`core/include/libvmaf/AGENTS.md`](../../core/include/libvmaf/AGENTS.md)
  — the public-headers-tree invariant note.
