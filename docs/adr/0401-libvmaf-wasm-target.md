<!-- markdownlint-disable MD060 -->
# ADR-0401: libvmaf WebAssembly target — phased EXPERIMENT then GO

- **Status**: Proposed
- **Date**: 2026-05-09
- **Deciders**: lusoris
- **Tags**: build, wasm, browser, ai, fork-local

## Context

WebAssembly is the platform-neutral compute target for code that
needs to run in browsers, Node.js, Deno, and Bun without a native
install. A WASM build of libvmaf would unlock four use cases the
native binary cannot serve:

- Streaming-service product engineers running VMAF on encoded
  variants directly inside their dev tools.
- Tooling and inspection sites embedding VMAF as a perceptual
  metric without a server round-trip.
- Educational content interactively demonstrating VMAF.
- Edge or low-spec devices with no GPU compute that still want a
  CPU-only score.

[Research-0089](../research/0089-libvmaf-wasm-feasibility-2026-05-09.md)
documents the feasibility study. Key constraints surfaced there:

- Emscripten + Meson cross-build is supported and does not require
  upstream changes to libvmaf or to Meson.
- WASM has no path to the fork's GPU backends (CUDA / SYCL /
  Vulkan / HIP / Metal); only the CPU path is portable.
- WASM-SIMD via simde's AVX/NEON shim covers the existing SIMD
  trees without per-feature rewrites; threading requires
  `SharedArrayBuffer` + `COOP`/`COEP` headers, so the build must
  also work single-threaded.
- ONNX Runtime ships an official `onnxruntime-web` package, but
  every shipped tiny-AI model needs an op-allowlist audit against
  the pinned ORT-Web release before a Tier-3 build.
- **The Netflix CPU golden-data gate (CLAUDE §8) cannot apply to
  WASM.** WASM math semantics, `musl` libm, and the optional
  `relaxed-simd` lane permit ULP drift that makes bit-exactness
  impossible in the general case. The WASM build joins the same
  class as GPU / SIMD: numerically close, never claimed
  bit-identical (memory `feedback_golden_gate_cpu_only`).

There is no in-process WASM VMAF on the npm registry today, so a
first-party fork build fills an empty slot rather than competing
with prior art.

## Decision

We will pursue a libvmaf WASM target in **three sequenced phases,
gated on an EXPERIMENT first**:

1. **EXPERIMENT (this ADR's only commitment).** Land a smallest-
   possible Tier-1 prototype (scalar CPU only, no SIMD, no AI
   head) on a feature branch behind a `meson_options.txt` flag
   `enable_wasm=false` by default. Goal: prove the
   Emscripten + Meson cross-build runs end-to-end and produces a
   `.wasm` that scores the 3 Netflix CPU pairs to within a
   tolerance the experiment itself defines (and reports). No
   release, no npm publish, no public commitment.
2. **Tier 2 (separate ADR after EXPERIMENT proves Tier 1).** Add
   simde + `wasm_simd128` for AVX2/NEON shimming, optional
   `pthread` build, and a CI lane `wasm-cpu` running a dedicated
   snapshot suite under `testdata/scores_wasm_*.json` (per
   CLAUDE §9 — this is *not* the Netflix gate). Publish to npm
   as `@lusoris/libvmaf-wasm` and attach a `.wasm` artifact to
   the `release-please` GitHub release.
3. **Tier 3 (separate ADR after Tier 2 ships).** Integrate
   `onnxruntime-web`, audit each shipped tiny-AI model against
   the pinned ORT-Web op allowlist, ship the saliency / fr-
   regressor heads as additional opt-in modules.

This ADR commits the project only to the EXPERIMENT (phase 1).
Tier 2 and Tier 3 each get their own ADR after the prior tier's
data lands.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **EXPERIMENT then phased GO (chosen)** | Prove buildability before promising anything; each tier ships its own ADR with empirical data; matches the fork's "no guessing" memory rule. | Defers a public WASM artifact by one EXPERIMENT cycle; no immediate marketing win. | — |
| **GO directly, ship Tier 1 + Tier 2 in a single PR** | One push, fastest user-visible delivery. | Bundles a build that's never been run with the docs / CI / packaging machinery; Netflix-golden-gate confusion (does WASM count?) compounds across surfaces; violates memory `feedback_no_guessing`. | Rejected — too many unknowns to commit upfront. |
| **NO-GO** | Zero new build target, zero new docs, zero new CI lane. | Closes the door on browser-side VMAF entirely; no in-process WASM VMAF exists on npm today, so the unfilled slot stays unfilled; loses the four use cases listed in Context. | Rejected — the use cases are real and the Emscripten path is plausibly small. |
| **Outsource: write a JS wrapper that calls a hosted server-side VMAF** | Trivial to implement; reuses native binary. | Loses every "no server" benefit; tooling sites and educational content need browser-local execution; not a VMAF artifact, just a network proxy. | Rejected — addresses none of the use cases. |
| **Pure JavaScript / AssemblyScript reimplementation of VMAF** | Smallest module, no WASM toolchain dependency. | Forks the codebase, tens of thousands of lines to port by hand, no Netflix-golden parity strategy. | Rejected — out of proportion to the value. |
| **WebAssembly + WebGPU compute kernels (write feature kernels in WGSL)** | GPU-accelerated browser VMAF, theoretically. | A from-scratch GPU backend; out of scope for "can we ship CPU-only WASM"; should be a post-Tier-3 ADR if demand justifies. | Deferred — wrong question for the feasibility study. |

## Consequences

- **Positive**: A successful EXPERIMENT unblocks Tier 2 / Tier 3
  with empirical numbers (build size, perf, ULP drift) instead
  of guesses. Browser-side VMAF becomes a credible roadmap item.
  Fills an empty slot on the npm registry.
- **Negative**: Adds a fourth build axis (CPU / CUDA / SYCL /
  WASM) the fork must keep green. Doc surface grows
  (`docs/development/wasm.md`, `docs/usage/wasm-quickstart.md`,
  per CLAUDE §12 r10). Lint scope grows
  (memory `feedback_no_lint_skip_upstream` — the WASM build's
  shims and bridge code lint with the same rules as native).
  Tier 3 is gated on a per-model op-allowlist audit that must
  re-run on each ORT-Web pin bump.
- **Neutral / follow-ups**:
  - The WASM build runs its **own** snapshot suite under
    `testdata/scores_wasm_*.json` per CLAUDE §9. It does **not**
    participate in the Netflix CPU golden-data gate
    (CLAUDE §8 / memory `feedback_golden_gate_cpu_only`); the
    experiment must document this loudly so a future contributor
    does not accidentally wire `make test-netflix-golden`
    against the WASM artifact.
  - The EXPERIMENT PR will be `experiment` scope, behind
    `enable_wasm=false`, and explicitly NOT a release-please
    feat / fix.
  - Each subsequent tier (2, 3) ships its own ADR with empirical
    numbers from the prior tier's deployment.

## References

- [Research-0089](../research/0089-libvmaf-wasm-feasibility-2026-05-09.md)
  — feasibility study; primary-source citations for every
  Emscripten / WASM-SIMD / ORT-Web claim.
- [CLAUDE.md §8](../../CLAUDE.md) — Netflix golden-data gate is
  CPU-only.
- [CLAUDE.md §9](../../CLAUDE.md) — snapshot regeneration policy
  (the WASM build gets its own snapshot file).
- [CLAUDE.md §12 r10](../../CLAUDE.md) — project-wide doc-substance
  rule (WASM ships docs in the same PR as the build).
- Memory `feedback_no_guessing` — every WASM platform claim cites
  a primary source.
- Memory `feedback_golden_gate_cpu_only` — WASM joins GPU/SIMD as
  "close but never bit-exact"; never claim WASM golden-equivalence.
- Source: `req` (paraphrased) — direct user direction:
  research-only feasibility study for compiling libvmaf to
  WebAssembly, with phased tier rollout, decision matrix as the
  deliverable, no implementation in this PR.
